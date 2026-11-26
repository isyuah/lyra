# Lyra RFC：HTTP 入站网络运行时

**状态：已接受。** 本文定义 `std.http` 与 `std.httpTls` 的 HTTP/1 入站网络、背压与生命周期，不改变出站 HTTP 客户端。

## 平台与线程归属

- 进程首次成功监听时启动一组 libuv Reactor。默认按可用 CPU 数取 `1..4`；`LYRA_HTTP_REACTORS` 可指定 `1..64`，非法值在 `listen` 返回 `Invalid`。这组线程与 CPU Worker、libuv 阻塞池独立。
- 监听 socket 只用 `uv_poll_t` 等待可接受事件，由所属 Reactor 分批取得原生 socket。新 socket 分配给当前在途连接最少的 Reactor，平局轮转；在目标 loop 首次 `uv_tcp_open`，之后不迁移。Linux 的流由 libuv 使用 epoll，Windows 使用 IOCP，不依赖 `SO_REUSEPORT` 或跨 loop `uv_accept`。
- listener 的接入、派发和计数在其所属 Reactor 上串行；每条连接的流、计时器、llhttp 和 TLS 状态只在其目标 Reactor 上变更。跨线程用带显式 GC roots 的 `uv_async_t` 命令传递。队列限制新操作的提交，取消、释放和关闭通知不丢弃；批量推进时将控制权交还 loop，让其他连接有机会运行。
- Reactor 不执行 Lyra handler、用户 codec、日志 sink 或阻塞数据库工作。它只把完成值交给 Task runtime，等待者在自己的执行域恢复。进程正常退出时停止接入并等待句柄关闭，再加入 Reactor 线程；入站 TLS 的清理先于 OpenSSL 全局退出。

## 接入、超时与背压

`listen` / `listenTls` 同步校验、绑定并返回实际端口，接收同一个 `HttpServerOptions`：

| 选项 | 默认值 | 作用 |
| --- | ---: | --- |
| `capacity` | 4 | 同时发放的请求，范围 `1..64` |
| `requestTimeoutMs` | 30000 | 从接入（TLS 为握手完成）到收到完整请求 |
| `idleReadTimeoutMs` | `None` | 读取请求期间距上次接收 HTTP 字节的上限 |
| `writeTimeoutMs` | 5000 | 从开始写响应到传输结束的预算 |
| `handshakeTimeoutMs` | 5000 | TLS 握手预算，独立于请求读取 |
| `graceMs` | 5000 | 优雅停机等待 handler 的预算，可为 0 |
| `claimTimeoutMs` | 5000 | 裸 `accept` 结果等候认领的预算 |

这些是库的可覆盖策略，不是语言规则。请求、写入、握手和认领预算为 `1..2147483647` 毫秒；空闲预算为 `None` 或同一范围。连接超时用单调钟定时器处理，不靠固定时间片重试 socket 操作。

每个 listener 最多同时持有 `2 × capacity` 条普通连接，计入握手、解析、待派发、已发放和关闭中的连接；关闭完成才释放名额。完整请求按 Reactor 收到就绪通知的顺序派发，不被较早到达的半条请求挡住。额外只有一个限时拒绝槽，尽力回 `503` 与 `Retry-After: 1`（TLS 先握手再加密回复）。拒绝槽也满时暂停接入，等待内核 backlog 与名额释放；没有无限排队或每连接线程。

llhttp 增量解析请求行、首部和正文，保持既有大小、UTF-8、`Host` 和 `Expect` 规则。一条连接只处理一条请求。读取超时可回 `408`；非法请求可回 `400`、`413` 或 `417`。已登记的手动接收可以观察单连接错误；Router 不因单条连接失败而关闭服务。

## Task、连接与停机

- `accept`、`respond`、`waitDisconnect` 和 `closeAndWait` 都创建冷 Task。`accept` 的 `timeoutMs` 控制等待已解析请求，`-1` 无期限、`0` 只取当前就绪项；只取消这个等待不会取消别的接收。裸结果是有期限的发放，调用 `claim()` 或开始写有效响应后认领。`withAccepted` 在发布请求前登记作用域清理，直到回调及其子任务退出、连接原生关闭完成才结束。
- 完整请求后的明文 TCP FIN 只代表读侧结束，仍允许响应。不完整请求 EOF、reset 和没有 `close_notify` 的 TLS 截断是对端断线。`waitDisconnect` 观察同一连接的事件，超时或本地关闭返回 `false`；取消观察不关闭连接。Router 用独立子 Task 观察断线，取消所属请求及其子任务，不影响其他请求。
- 每条连接最多一个挂起的 `uv_write`；大响应分块，正文直接借用保活的 `Bytes`。写入竞争只有一个胜者。取消写入或显式关闭会停止读写并关闭句柄，等 write callback 和 close callback 回收缓冲、TLS 状态及 roots 后才完成相关 Task。SDK 私有桥接在投递前登记等待和执行器保活；普通 `extern` 仍不接受 `Task`。
- TLS 1.2/1.3 用双向有界的 OpenSSL memory BIO pair 与同一个 `uv_tcp_t` 交换密文。握手、解密和加密只在有输入、写完成或有明确进展时推进；输出阻塞时保留至多一份待写的密文分块。响应结束发送 `close_notify`，不会无期限等待对端关闭。明文程序不链接 TLS。
- `close()` 同步、幂等地请求关闭；`closeAndWait()` 等待原生资源回收。`Router.serve(listener, shutdown: …)` 使用 listener 的容量和宽限期。收到 `Shutdown.request()`，先停止接入、关闭未认领连接，再等已认领的 handler；到期取消剩余任务，仍等 SQLite 等原生工作安全返回。直接取消服务跳过宽限期。监听器关闭确认还包括连接及 TLS context 释放。

## 验收

- Windows／Linux Debug 和 Release 检查请求分片、FIN 与 reset、大响应背压、慢客户端、过载、认领超时、竞争、GC 与有界停机；`UV_THREADPOOL_SIZE=1` 时，忙碌的 SQLite 或其他阻塞工作不挡住健康响应。
- 本地临时 CA 验证 TLS 1.2/1.3、证书失败、握手超时、截断和关闭，不依赖公网。用原生门闩在两个 Reactor 上确认连接实际由不同 OS 线程推进，且释放最后一条连接前不释放它所用的 TLS context。
- 同一负载对比旧 `poll/WSAPoll` 路径与单／多分片 Reactor，记录吞吐、延迟分位和进程 CPU。该测量用于观察回归，不把特定机器的默认分片数当作性能保证。
