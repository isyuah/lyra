# Lyra RFC：HTTP 网络运行时

**状态：实现方向已接受，运行时改造待分阶段落地。**
**范围：** `std.http` 与 `std.httpTls` 的入站 HTTP/1 连接、异步任务交接、超时、取消、过载和停机。出站 HTTP 客户端不在此 RFC 范围内。

## 现状与目标

当前入站路径在 Linux 使用 `poll()`，Windows 使用 `WSAPoll()`；调用方以至多 20–25ms 的等待切片重复检查取消与截止时间。系统调用会睡眠，因此这不是忙循环，但每个等待操作会占用一个阻塞 Worker、反复唤醒并重新检查单个 socket。它不适合作为高并发服务器的长期架构。

Lyra SDK 已依赖 libuv。网络运行时使用 libuv 的**流式 TCP API**（`uv_tcp_t`、`uv_read_start`、`uv_write`、`uv_timer_t`），而不是在每个连接上建立阻塞等待，也不把 `uv_poll_t` 当作通用 TCP 流封装。Linux 由 libuv 使用 epoll 后端；Windows 使用其原生异步 socket 后端（IOCP）。Lyra 网络等待不占用语言 Worker 或 libuv 阻塞线程池。

## 所有权与事件循环

- 进程内所有入站 HTTP listener 共用一个网络 Reactor，避免每个服务/连接各自创建线程或 loop。其 libuv loop 及全部 listener、connection、timer、TLS 和 parser 状态只由 Reactor 线程访问；进程退出时在所有服务停止并完成句柄关闭后再销毁。
- 跨线程只投递有界控制命令（开始监听、关闭、取消、发送响应）；通过 `uv_async_t` 唤醒 Reactor。不得从 Lyra Worker 直接读写 `uv_tcp_t`、`SSL*`、llhttp 状态或连接缓冲区。
- Reactor 线程注册到 BDWGC。它只通过显式 GC roots 持有 Lyra Task、Request 和响应值；每个完成、取消及关闭路径恰好释放相应 root。
- I/O 完成由 Reactor 发布到 Task runtime。Task runtime 在锁内登记结果并唤醒所属执行域；用户 handler 永远不在 Reactor 回调中执行。完成值保持到 Task 消费或终态回收。
- listener 与 connection 的本地状态只有 Reactor 可变。Task 侧的公开句柄使用稳定身份和原子终态，重复关闭幂等；并发响应和已关闭句柄返回确定的 `HttpError`。

## 连接状态机

每条连接严格按以下状态转移：

```text
Accepted -> Handshaking? -> ReadingHeaders -> ReadingBody
         -> RequestReady -> Handling -> WritingResponse -> Closing -> Closed
```

任何非终态都可收到超时、客户端断开、服务停机或 handler 取消。转入 `Closing` 后停止读写并调用 `uv_close`；在所有未决 write callback 与 close callback 到达前不释放原生 connection、TLS 对象、GC roots 或缓冲。重复取消和重复关闭不重复完成 Task，也不释放两次。

llhttp 在 read callback 中增量消费任意分片；不要求请求行、头或正文与 socket read 边界对齐。保留既定的线、头、目标、字段数和正文大小上限；解析只在有输入时推进，不以定时重试等待数据。解析和 TLS 驱动按回调预算分批，让出 loop，避免单个大请求长期占住所有连接的进度；用户 handler、日志 sink 和业务 codec 不在 Reactor 上运行。

## Lyra Task 与取消

- `Listener.accept` 与 `Request.respond` 作为 Task 操作接入语言既有冷 Task 语义；构造操作不启动网络 I/O，首次 `await`/`spawn` 才登记请求。取消通过 Reactor 命令撤销尚未提交的操作。
- Task 取消只表示请求停止，不意味着底层 I/O 已结束。网络 Task 在对应连接进入 `Closed`、所有 libuv callback 结束、句柄和 roots 安全释放后才发布取消终态。任务组因此可以安全收尾，不会把正在被原生回调引用的资源交还给调用者。
- HTTP handler 的断线监听复用该连接的 read/close 事件，取消当前请求 Task 及其子任务；不另建每连接轮询器。普通 `blocking` C 工作仍按异步 RFC 等待返回后再关闭其句柄。
- 同步 `close()` 仅可将关闭命令投递给 Reactor 并立即使 Lyra 句柄失效；需要确认连接已释放的服务停机流程使用 Task 等待 close completion。不存在从 Reactor 回调同步等待 Reactor 自己的路径。

## TLS

TLS 连接也由同一 `uv_tcp_t` 收发。OpenSSL 使用 memory BIO：网络 read callback 把密文写入 read BIO，状态机驱动 `SSL_accept` / `SSL_read`；`SSL_write` 输出从 write BIO 取出，再由 `uv_write` 发送。每个 TLS 状态只在 Reactor 上访问。

握手、空闲读、整条请求、响应写入分别使用 `uv_timer_t` 截止时间。握手超时会关闭连接并记录现有 TLS 错误类别；不会阻塞 Worker，也不会用 `poll()` 等待 WANT_READ/WANT_WRITE。TLS 1.2/1.3、证书配置校验和应用层 HTTP 解析契约保持不变。

## 有界并发、背压与停机

- `Router.serve(capacity)` 限制同时运行的 handler 数量；Reactor 另有明确的待解析连接队列上限。队列满时按确定的 overload 响应（`503 Service Unavailable`，带 `Retry-After`）关闭新连接，绝不无界接受后再缓存请求正文。
- accept 后立即设置连接级期限和大小预算。超过队列容量、握手期限、请求期限或内存预算的连接不占用 handler 槽，并在安全关闭后释放槽位。
- 正常停机先停止 accept，再阻止新的请求进入 handler 队列，等待已进入的 handler 与响应完成。宽限期到期后取消剩余 handler，等待正在运行的 blocking 工作、libuv callback 和句柄 close 完成，最后释放 TLS context 与 Reactor。
- `listen` / `close`、超时和取消命令在关闭竞争下以 Reactor 上实际处理顺序确定结果；不得靠重复调用关闭函数或短 sleep 轮询 `Busy` 来实现停机。

## 验收门槛

每个功能切片均运行 Linux 与 Windows 的 Debug 定向测试；Reactor 改造完成后运行两端 Debug/Release 全量 CTest。

- TCP：逐字节分片请求行/头/正文、多个分片、客户端半关闭与 reset、空闲慢客户端、响应部分写、取消与关闭竞争、超过连接队列上限、并发洪峰及停机期间连接。
- TLS：TLS 1.2/1.3、分片 ClientHello/握手记录、握手超时、异常证书、加密正文分片、响应部分写、取消时 write 未完成、断连清理和 listener 关闭期间释放 SSL context。
- 并发与容量：`UV_THREADPOOL_SIZE=1` 下慢客户端、CPU 任务、SQLite blocking 工作混跑；请求槽和待解析连接均有界；过载时健康请求与停机仍可推进。
- 生命周期：以闩锁确定 callback 与取消/close 的竞态顺序；断言 handler 清理先于组完成，原生回调结束前 roots 不释放，listener 完成前 context 不释放；同一连接只能发布一个终态。
- 效率：使用固定请求集同时报告吞吐、p50/p95/p99、进程 CPU 与活动连接数。基线 `poll/WSAPoll` 的结果仅供比较，不作为 Reactor 达标数据；不以缩短 timeout 或提高 Worker 数代替架构验收。

## 分阶段实施

1. 建立 libuv TCP Reactor、线程命令队列和确定的启动/停止生命周期；先覆盖明文 listener 与连接 read/write。
2. 让 llhttp 解析和 `Listener.accept` / `Request.respond` 通过冷 Task 完成；增加 Task 取消交接和连接 root 生命周期，迁移同步示例与测试。
3. 在同一 Reactor 上加入有界连接队列、请求截止时间、过载响应和真正等待 close completion 的优雅停机。
4. 将服务端 TLS 改为 memory BIO + libuv 流状态机，删除服务端握手、读写路径上的 `poll/WSAPoll`。
5. 完成双平台并发与生命周期验收，再与旧实现跑同负载对照；旧路径全部迁移后删除阻塞式服务端实现和过时测试夹具。

## 明确不采用

- 把轮询片间隔从 20ms 调到 1ms，或为每个连接增加一个 `poll()` 线程。
- 在每个连接上启动一个 `uv_loop` 并同步等待；这虽能借 libuv 分发 socket readiness，仍会按并发连接占满 Worker。
- 对每个 socket 使用 `uv_poll_t` 代替 `uv_tcp_t`，或让 OpenSSL 阻塞等待 readiness。
- 无界请求队列、取消时立即释放仍有原生 callback 使用的状态、在 Reactor callback 中运行用户 handler。
