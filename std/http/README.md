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

`listen(0)` 绑定 `127.0.0.1` 的临时端口，`port()` 读取实际值。`accept(timeoutMs)` 是同步操作，超时覆盖等待连接及读完整请求。`status()` 为零表示可用，失败为 Linux `errno`。`target()` 保留请求目标和查询串；`pathInt(prefix, fallback)` 解析指定前缀后一个非负十进制参数，自动忽略查询串。`body()` 是经过 UTF-8 验证的文本，可含 NUL，支持分块传输。

一条 TCP 连接一个 HTTP 请求。请求线、头部和传输体总计最多 256 KiB，目标最多 8 KiB，解码后正文最多 128 KiB；超限回 `413`，格式或文本非法回 `400`。`respond()` 发送状态、Content-Type、Content-Length 和正文后关闭连接，返回零或 `errno`；`close()` 可显式放弃或释放。同步 API 只在同一执行线程使用，未提供 TLS、HTTP/2、长连接、生产级并发调度。完整的 [SQLite 服务](../../examples/http-sqlite/README.md) 展示成功、找不到和数据库故障的区别。

解析器为静态链接的官方 llhttp 9.3.0。开发机首次配置会下载带 SHA-256 校验的**生成版 C 源码**到 `$XDG_CACHE_HOME/lyra/toolchain`（未设置则 `$HOME/.cache/lyra/toolchain`）。以后两个预设及多个工作区共享缓存。离线配置用 `cmake --preset dev -DLYRA_LLHTTP_OFFLINE=ON`；全新离线环境设置 `-DLYRA_LLHTTP_SOURCE_DIR=/path/to/llhttp-release-v9.3.0`。约束使用上游 `release/v9.3.0` 的归档，而不是需要 Node.js 生成 C 的普通源码标签。应用部署不需要 Node.js、动态 llhttp 或编译器。
