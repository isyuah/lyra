# Lyra RFC：HTTP 入站网络运行时

**状态：已接受。** 本文定义 `std.http` 与 `std.httpTls` 的 HTTP/1、HTTP/2 入站网络、背压与生命周期。出站 HTTP 客户端复用相同的静态协议依赖。

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
| `requestTimeoutMs` | 30000 | 首次接入（TLS 为握手完成）或复用连接收到首个字节后，到收到完整请求 |
| `idleReadTimeoutMs` | `None` | 读取请求期间距上次接收 HTTP 字节的上限 |
| `writeTimeoutMs` | 5000 | 从开始写响应到传输结束的预算 |
| `handshakeTimeoutMs` | 5000 | TLS 握手预算，独立于请求读取 |
| `graceMs` | 5000 | 优雅停机等待 handler 的预算，可为 0 |
| `claimTimeoutMs` | 5000 | 裸 `accept` 结果等候认领的预算 |
| `maxBodyBytes` | 8388608 | 解码传输分块后的请求正文上限 |
| `maxRequestBytes` | 16777216 | 单条请求的请求线、首部、传输分块与正文总上限 |
| `keepAliveTimeoutMs` | 5000 | 两条请求之间等待首字节的预算，0 关闭连接复用 |
| `maxRequestsPerConnection` | 1000 | 单条连接最多处理的请求数 |
| `streamRequestBody` | `false` | 首部就绪即派发，handler 按需读取正文 |
| `enableHttp2` | `true` | TLS 通过 ALPN 优先协商 HTTP/2 |

这些是库的可覆盖策略，不是语言规则。请求、写入、握手和认领预算为 `1..2147483647` 毫秒；空闲预算为 `None` 或同一范围。连接超时用单调钟定时器处理，不靠固定时间片重试 socket 操作。

每个 listener 最多同时持有 `2 × capacity` 条普通连接，计入握手、解析、待派发、已发放和关闭中的连接；关闭完成才释放名额。请求按 Reactor 收到就绪通知的顺序派发，不被较早到达的半条请求挡住。额外只有一个限时拒绝槽，尽力回 `503` 与 `Retry-After: 1`（TLS 先握手再加密回复）。拒绝槽也满时暂停接入，等待内核 backlog 与名额释放；没有无限排队或每连接线程。

llhttp 增量解析请求行、首部和正文，检查 UTF-8、`Host` 和 `Expect`。HTTP/1.1 默认复用连接，HTTP/1.0 需要显式 `Connection: keep-alive`。同一连接的请求按顺序执行，最多额外缓存一个 8 KiB 读取窗口的管线数据，其余通过暂停 socket 读取产生背压。达到请求次数上限、客户端要求关闭、半关闭或服务进入停机时，当前响应后关闭连接。空闲连接仍占普通连接名额，到期直接回收。

每条请求都有独立的 `Request` 对象，物理连接由独立的 Session 管理。完成响应后，旧 Request 的缓冲正文、首部与路由绑定仍可读取，旧对象的关闭、流式读取和写入无法操作下一条请求。声明超过正文限制的 `Content-Length` 在首部完成后即拒绝。拒绝时先写出错误并关闭写侧，再限时消费剩余输入，减少未读上传数据导致的 TCP reset。

默认缓冲模式在完整请求后派发；流式模式只等待完整首部。流式正文按 `readBody()` 的需求推进 llhttp，并在每个读取窗口后把不可变字节块交给 handler，未登记下一次读取就停读。每次读取最多 8 KiB，只允许一个挂起读取。累计字节和总超时始终生效；故意停读时不计网络空闲超时。提前响应时关闭连接，写端结束后限量、限时排空。取消挂起读关闭当前连接并等待原生确认。

读取超时可回 `408`，非法请求可回 `400`、`413` 或 `417`。已登记的手动接收可以观察单连接错误，Router 继续处理其他请求。

## Task、连接与停机

- `accept`、`respond`、`waitDisconnect` 和 `closeAndWait` 都创建冷 Task。`accept` 的 `timeoutMs` 控制等待已解析请求，`-1` 无期限、`0` 只取当前就绪项；只取消这个等待不会取消别的接收。裸结果是有期限的发放，调用 `claim()` 或开始写有效响应后认领。`withAccepted` 在发布请求前登记作用域清理，回调及子任务退出后，等当前请求释放再结束。响应成功后可复用连接，未响应或失败时关闭物理连接。
- 完整请求后的明文 TCP FIN 只代表读侧结束，仍允许响应。不完整请求 EOF、reset 和没有 `close_notify` 的 TLS 截断是对端断线。`waitDisconnect` 观察当前请求的连接事件，超时、本地关闭或当前响应完成返回 `false`；取消观察不关闭连接。Router 用独立子 Task 观察断线，取消所属请求及其子任务，不影响其他请求。
- 每条连接最多一个挂起的 `uv_write`；大响应分块，正文直接借用保活的 `Bytes`。写入竞争只有一个胜者。取消写入或显式关闭会停止读写并关闭句柄，等 write callback 和 close callback 回收缓冲、TLS 状态及 roots 后才完成相关 Task。SDK 私有桥接在投递前登记等待和执行器保活；普通 `extern` 仍不接受 `Task`。
- `Response.stream` 在响应头完成后于 handler 域运行生产函数。`ResponseWriter.write` 逐块等待同一 Reactor 的原生写入，未完成时拒绝第二次写入。HTTP/1.1 用 chunked 定界，HTTP/1.0 用关闭定界。正常返回时由库结束正文；生产失败、断线或取消终止传输。`HEAD` 不执行生产函数，旧 writer 不能用于后续请求。
- TLS 1.2/1.3 用双向有界的 OpenSSL memory BIO pair 与同一个 `uv_tcp_t` 交换密文。握手、解密和加密只在有输入、写完成或有明确进展时推进；输出阻塞时保留至多一份待写的密文分块。连接复用共享已完成的 TLS 握手，关闭连接时发送 `close_notify`。明文程序不链接 TLS。
- `close()` 同步、幂等地请求关闭；`closeAndWait()` 等待原生资源回收。`Router.serve(listener, shutdown: …)` 使用 listener 的容量和宽限期。收到 `Shutdown.request()`，先停止接入、关闭未认领连接，再等已认领的 handler；到期取消剩余任务，仍等 SQLite 等原生工作安全返回。直接取消服务跳过宽限期。监听器关闭确认还包括连接及 TLS context 释放。

## HTTP/2

TLS ALPN 默认依次提供 `h2`、`http/1.1`，禁用 `enableHttp2` 时只提供后者。未发送 ALPN 的连接使用 HTTP/1。HTTP/2 协议、HPACK、帧合法性和流状态由静态 nghttp2 1.70.0 管理，明文入口保持 HTTP/1。TLS 1.2 使用 ECDHE 与 AEAD 密码套件，TLS 1.3 使用 OpenSSL 默认套件。

- 每个流拥有独立 Request、GC roots、读写等待、断线观察、认领和请求截止时间。请求就绪与 handler 容量由监听器统一派发。每条连接最多 `capacity` 个活跃流，整个 listener 最多 `2 × capacity` 个活跃 HTTP/2 请求；超限流以 `REFUSED_STREAM` 拒绝。
- 请求方法、目标和可见字段使用与 HTTP/1 相同的校验；`:authority` 映射为 `Host`，同时提供时必须一致。禁止逐跳连接字段，保留重复普通字段的顺序。`maxBodyBytes` 限制 DATA 正文，`maxRequestBytes` 累计请求的 HEADERS、CONTINUATION、DATA 帧及其帧头。解压字段另受 32 KiB 与 100 项限制，字段空间计入每项 32 字节开销。HPACK 动态表固定上限 4096 字节，ACK、SETTINGS、CONTINUATION 与流重置另有队列和速率限制。
- 流式请求的接收窗口为每流 65535 字节。每次 `readBody()` 最多返回 8 KiB，应用取走后补回该流的窗口。连接窗口随 DATA 接收补回，应用暂停一个流时其他流仍可接收。默认缓冲模式在完整正文后派发，累计大小仍受配置限制。
- 输出每次最多暴露 16 KiB，等待该分块对应的 TLS 密文 `uv_write` 回调完成后才确认消费。每流同时只允许一个挂起写入；完成信号取走后才接受下一块。传输背压、连接窗口和流窗口共同约束输出。
- 请求截止时间在对端 END_STREAM 后停止，尚未消费的已接收正文仍可读取。网络空闲计时只在等待读取时生效，写入和认领分别计时。新连接等待首个请求使用请求预算，流之间的空闲使用 `keepAliveTimeoutMs`。流超时重置所属流；物理传输写入超时关闭连接并收尾所有流。
- 取消 handler、请求或 writer 发送所属流的 RST_STREAM，完成该流的原生等待后再释放 roots。提前响应完整发送后用 `RST_STREAM(NO_ERROR)` 停止剩余上传。旧 Request 无法操作其他流。物理连接关闭时先终止所有流并完成原生等待，再释放 TLS 和连接句柄。
- 优雅停机发送带最后已接受流 ID 的 GOAWAY，停止接受新流，取消未认领流，让已认领流在宽限期内完成。收到对端 GOAWAY 同样收尾。达到 `maxRequestsPerConnection` 或 `keepAliveTimeoutMs == 0` 时发送 GOAWAY，后者每连接只接受第一个请求。
- 正常结束先确认 GOAWAY 和应用响应已写入传输，再发送 TLS `close_notify` 并关闭 TCP 写侧。读侧继续在 Reactor 上排空最后到达的控制帧密文，直到 EOF、1 秒或收到 64 KiB，避免因未读入站数据导致 TCP reset 丢失已发尾帧。强制关闭和宽限期到期不等待这段自然收尾。

协议引擎只由所属 Reactor 调用。创建回调在 nghttp2 调用栈中分配 Request 与所属状态；返回空指针表示拒绝该流。其他回调在 nghttp2 返回后登记就绪、进度或关闭事件，随后由 Reactor 完成 Task。关闭事件优先于同批进度事件，引擎在关闭通知后释放自身 Request root。每次输出视图在确认消费前保持有效，确认后才可继续生成输出。

## 验收

- Windows／Linux Debug 和 Release 检查请求分片、FIN 与 reset、大响应背压、慢客户端、过载、认领超时、竞争、GC 与有界停机；`UV_THREADPOOL_SIZE=1` 时，忙碌的 SQLite 或其他阻塞工作不挡住健康响应。
- 本地临时 CA 验证 TLS 1.2/1.3、证书失败、握手超时、截断和关闭，不依赖公网。用原生门闩在两个 Reactor 上确认连接实际由不同 OS 线程推进，且释放最后一条连接前不释放它所用的 TLS context。
- HTTP/2 用本地 CA 验证 ALPN、真实 SDK 客户端 HPACK 解码、并发流、双向流控、RST 隔离、未读完就响应、GOAWAY 与 HTTP/1 回退；零窗口流暂停期间健康流仍成功，关闭复用后首条流完整结束。
- 同一负载对比旧 `poll/WSAPoll` 路径与单／多分片 Reactor，记录吞吐、延迟分位和进程 CPU。该测量用于观察回归，不把特定机器的默认分片数当作性能保证。
