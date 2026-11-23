# std.http：本地 HTTP/1 监听

```text
import std.http.listen
fn main(): Unit {
    val server = listen(8080)
    if server.status() != 0 { return }
    val request = server.accept(5000)
    if request.status() == 0 { request.respond(200, "text/plain; charset=utf-8", "你好"); }
    server.close()
}
```

`listen(0)` 绑定 `127.0.0.1` 的临时端口，`port()` 读取实际值。`accept(timeoutMs)` 是同步操作，超时覆盖等待连接及读完整请求。`status()` 为零表示可用，失败带本平台的网络或 I/O 错误码。`target()` 保留请求目标和查询串；`pathInt(prefix, fallback)` 解析指定前缀后一个非负十进制参数，自动忽略查询串。`body()` 是经过 UTF-8 验证的文本，可含 NUL，支持分块传输。

一条 TCP 连接一个 HTTP 请求。请求线、头部和传输体总计最多 256 KiB，目标最多 8 KiB，解码后正文最多 128 KiB；超限回 `413`，格式或文本非法回 `400`。`respond()` 发送状态、Content-Type、Content-Length 和正文后关闭连接，返回零或 `errno`；`close()` 可显式放弃或释放。同一 `Listener` 或 `Request` 的同步操作应串行调用。没有 TLS、HTTP/2 或长连接。

在异步 handler 中，`request.watchDisconnect()` 把当前请求的套接字登记到**当前 Task**：注册成功返回 `true`，随后断线会取消这个 Task 及它的子任务。在同步 `main` 或阻塞池线程内没有当前异步请求，注册会失败。`disconnected()` 只查询当下状态；`waitDisconnect(timeoutMs)` 同步等待状态变化，不传播取消。当前实现把对端关闭写方向也视为断线，客户端应在读取响应后再关闭连接。

可能阻塞的 `accept`、数据库访问和响应写入可放在 `await blocking { ... }` 中。把断线监听挂到每个请求自己的子任务，不要挂到整个服务的根任务；在请求组收尾后再关闭原始请求句柄。完整的 [SQLite 服务](../../examples/http-sqlite/README.md) 演示了这条路径。

解析器为静态链接的官方 llhttp 9.3.0。开发机首次配置会下载带 SHA-256 校验的**生成版 C 源码**到 `$XDG_CACHE_HOME/lyra/toolchain`（未设置则 `$HOME/.cache/lyra/toolchain`）。以后两个预设及多个工作区共享缓存。离线配置用 `cmake --preset dev -DLYRA_LLHTTP_OFFLINE=ON`；全新离线环境设置 `-DLYRA_LLHTTP_SOURCE_DIR=/path/to/llhttp-release-v9.3.0`。约束使用上游 `release/v9.3.0` 的归档，而不是需要 Node.js 生成 C 的普通源码标签。应用部署不需要 Node.js、动态 llhttp 或编译器。
