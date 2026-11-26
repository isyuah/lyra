# std.http：有界并发 HTTP/1 服务端

```lyra
import std.http.{listen}
import std.http.{Router}
import std.http.{Request}
import std.http.{Response}

async fn main(): Unit {
    val server = match listen(8080, host: "0.0.0.0") {
        Ok(listener) => listener,
        Err(error) => throw Exception(error.message()),
    }
    val router = Router()
    router.get("/hello/{name}", async (request: Request) => {
        Response.text(200, "你好 " + (request.path("name") ?? ""))
    })
    await router.serve(server, capacity: 4)
}
```

`listen(port, host: "127.0.0.1")` 返回 `Result<Listener, HttpError>`。`host` 使用数值 IPv4 或 IPv6 地址；如需对外监听，显式用 `0.0.0.0` 或 `::`，分别开放对应协议。传 `0` 让系统分配端口，`port()` 读取实际值。`Router.serve(listener, capacity, shutdown, graceMs)` 接管监听器，用固定接收槽并发处理路由，容量必须在 1..64。`shutdown` 默认为 `None`，`graceMs` 默认为 5000。

不用 Router 时，同步代码用 `listener.accept(timeoutMs): Result<Request, HttpError>`，成功后自行关闭请求。异步代码用 `await listener.withAccepted<T>(timeoutMs, handler)`，其中 `handler` 的类型为 `(Request) -> Task<T>`：

```lyra
val handled = await server.withAccepted<Unit>(5000, async (request: Request) => {
    match await blocking { request.respond(Response.text(200, "ready")) } {
        Ok(_) => {},
        Err(error) => throw Exception(error.message()),
    }
})
```

两者的超时都覆盖等待连接和读取完整请求。`withAccepted` 接受 `1..2147483647` 毫秒，返回 `Result<T, HttpError>`；回调成功、抛异常或取消后均关闭请求。接收过程中取消也会清理尚未交付的连接。不要从 `blocking { listener.accept(...) }` 直接返回请求，取消可能丢弃尚未由调用者接管的句柄。

## 请求、路由、响应

- `target()` 保留原始目标和查询串。`path(name)` 读取路由模板绑定，`pathInt(name)` 区分未绑定、合法整数、非法或越界的值。`queryAll(name)` 保留重复值及出现顺序。
- `header(name): Result<String?, HttpError>` 读取单值头，缺失为 `Ok(None)`、重复为 `DuplicateHeader`；`headers(name)` 按出现顺序取全部。字段名按 ASCII 大小写无关匹配。`cookie(name)` 查找 `Cookie` 内同名项，Cookie 名区分大小写。请求头必须是 UTF-8；分块传输的尾部字段不作为请求头。
- `body()` 提供原始 `Bytes`，保留 NUL 和非 UTF-8 字节；`text()` 显式验证 UTF-8，`json(codec)` 在此基础上按 codec 解码，两者都返回 `Result`。用 `Response.text`、`Response.bytes` 或 `Response.json` 创建响应；`withHeader` 返回带新增字段的独立响应。响应写出时验证字段名和值，`Content-Length`、`Content-Type`、`Connection` 等由库维护。
- `Router.get/post/put/delete/patch/options/head(pattern, handler)` 都调用 `route(method, pattern, handler)`；方法区分大小写。路由按注册顺序匹配，同一路径上的显式 `HEAD` 优先于回退到 `GET`，两者都只发送响应头。存在路径但方法不符返回 `405` 和 `Allow`，不存在路径返回 `404`。
- `#post("/teams/{id}")` 生成调用 `router.route("POST", ...)` 的 `<function>Route(router)`，由应用显式挂载。`[path]` 支持 `Int`/`String`；`[query("tag")]` 的 `String` 要求恰好一次，`List<String>` 保留所有值；`[body]` 使用同模块可见的 `jsonCodec<Type>()`，也可写 `[body("codecFactory")]` 指定零参数工厂。参数不符或 JSON 解码失败回 `400`，不调用业务函数。

一条连接一个 HTTP 请求。请求线、头部和传输体总计最多 256 KiB，目标最多 8 KiB，头部最多 100 项且字段名和值合计最多 32 KiB，解码后正文最多 128 KiB。超限回 `413`，格式或头部文本非法回 `400`。HTTP/1.1 必须有且只有一个有效的 `Host`；携带 `Expect` 的请求在读完首部后直接回 `417`，不等待正文。`request.respond(response)` 写出响应后关闭连接，返回 `Result<Unit, HttpError>`；响应在写前验证失败可改正后重试。TLS 见 [`std.httpTls`](../httpTls/README.md)，暂不支持 HTTP/2 和长连接。

## 关闭与断线

同一监听器可并行接受多个连接。传入 `Shutdown()` 后，可从信号处理任务调用 `request()`：服务停止接收新连接、关闭监听器，并等待已接收请求最多 `graceMs` 毫秒；到期后取消仍在运行的 handler，等待其清理和原生工作收尾，再返回。随后服务拥有并关闭监听器。没有传 shutdown 时，可取消服务任务并等待它收尾。手动关闭前，应停止并等待该监听器的 `accept` 操作结束；占用中的 `close()` 抛出 `std.http.Busy`，不会关闭仍在使用的句柄。请求同样会在响应写入与其他原生操作冲突时返回 `HttpErrorKind.Busy`，关闭与写入冲突时抛出 `Busy`。在关闭完成后再次 `close()` 无副作用。

```lyra
val shutdown = Shutdown()
val serving = spawn router.serve(listener, capacity: 4, shutdown: Some(shutdown), graceMs: 5000)
await waitForTerminationSignal()
shutdown.request()
await serving
```

`Shutdown.request()` 可重复调用；该对象用于在异步任务间传递停机信号，不代表操作系统信号处理器。

在异步 handler 中，`request.watchDisconnect()` 把当前请求登记到**当前 Task**：注册成功返回 `true`，随后对端断开会取消这个 Task 及它的子任务。本地 `close()` 或响应完成不会取消 Task。在同步 `main` 或阻塞池线程内无法登记。`disconnected()` 查询当前连接状态；`waitDisconnect(timeoutMs)` 同步等待，在 `blocking` 中会响应任务取消，不必等到超时。当前实现把对端关闭写方向也视为断线，客户端应在读取响应后再关闭连接。

数据库访问和响应写入等同步工作可以放在 `await blocking { ... }` 中。把断线监听挂到每个请求自己的子任务，不要挂到整个服务的根任务；在请求组收尾后再关闭请求。完整的 [SQLite 服务](../../examples/http-sqlite/README.md) 演示了这条路径。

解析器为静态链接的官方 llhttp 9.3.0。开发机首次配置会下载带 SHA-256 校验的**生成版 C 源码**到 `$XDG_CACHE_HOME/lyra/toolchain`（未设置则 `$HOME/.cache/lyra/toolchain`）。以后两个预设及多个工作区共享缓存。离线配置用 `cmake --preset dev -DLYRA_LLHTTP_OFFLINE=ON`；全新离线环境设置 `-DLYRA_LLHTTP_SOURCE_DIR=/path/to/llhttp-release-v9.3.0`。约束使用上游 `release/v9.3.0` 的归档，而不是需要 Node.js 生成 C 的普通源码标签。应用部署不需要 Node.js、动态 llhttp 或编译器。
