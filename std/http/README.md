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
    await router.serve(server)
}
```

`listen(port, host: "127.0.0.1", options: HttpServerOptions())` 返回 `Result<Listener, HttpError>`。`host` 使用数值 IPv4 或 IPv6 地址；如需对外监听，显式用 `0.0.0.0` 或 `::`。端口 `0` 让系统分配，`port()` 读取实际值。`Router.serve(listener, shutdown: …)` 接管监听器，使用 `options.capacity`（默认 4，范围 1..64）限制同时处理的请求。容量 `N` 的 listener 最多持有 `2N` 条普通连接和一个限时拒绝槽，过载尽力返回 `503` 与 `Retry-After: 1`。

不用 Router 时，推荐 `await listener.withAccepted<T>(timeoutMs, handler)`，其中 `handler` 的类型为 `(Request) -> Task<T>`：

```lyra
val handled = await server.withAccepted<Unit>(5000, async (request: Request) => {
    match await request.respond(Response.text(200, "ready")) {
        Ok(_) => {},
        Err(error) => throw Exception(error.message()),
    }
})
```

`withAccepted` 在回调及其子任务退出后，等待当前请求释放再结束。成功响应可复用连接，异常、取消或未响应时关闭连接。`await listener.accept(timeoutMs)` 返回裸请求，若不立即响应，调用 `request.claim()` 认领，并自行关闭。请求默认在发放 5000ms 后仍未被认领就关闭。两种接收的 `timeoutMs` 控制等待已解析请求，`-1` 无期限，`0` 只取当前就绪项，已登记的手动接收也可能得到客户端解析或握手错误。请求接入后的独立计时见 [`HttpServerOptions`](../../rfc/http-reactor.md#接入超时与背压)。

## 请求、路由、响应

- `target()` 保留原始目标和查询串。`path(name)` 读取路由模板绑定，`pathInt(name)` 区分未绑定、合法整数、非法或越界的值。`queryAll(name)` 保留重复值及出现顺序。
- `header(name): Result<String?, HttpError>` 读取单值头，缺失为 `Ok(None)`、重复为 `DuplicateHeader`；`headers(name)` 按出现顺序取全部。字段名按 ASCII 大小写无关匹配。`cookie(name)` 查找 `Cookie` 内同名项，Cookie 名区分大小写。请求头必须是 UTF-8；分块传输的尾部字段不作为请求头。
- `body()` 提供原始 `Bytes`，保留 NUL 和非 UTF-8 字节；`text()` 显式验证 UTF-8，`json(codec)` 在此基础上按 codec 解码，两者都返回 `Result`。用 `Response.text`、`Response.bytes` 或 `Response.json` 创建响应；`withHeader` 返回带新增字段的独立响应。响应写出时验证字段名和值，`Content-Length`、`Content-Type`、`Connection` 等由库维护。
- `Router.get/post/put/delete/patch/options/head(pattern, handler)` 都调用 `route(method, pattern, handler)`；方法区分大小写。同方法的路径同时匹配时，从左往右比较，在首个有区别的段优先选字面量。显式 `HEAD` 优先于回退到 `GET`，两者都只发送响应头。存在路径但方法不符返回 `405` 和 `Allow`，不存在路径返回 `404`。
- `#get` 和 `#post` 在 `src/` 下的 `async fn` 上生成私有适配器。启动时写一次 `#mountRoutes(router, AppDeps(...))`，按当前包收集各模块的路由；没有依赖时写 `#mountRoutes(router)`。`[inject]` 读取依赖对象的同名可见成员，类型必须精确匹配（透明别名按原类型匹配）；路由器和依赖表达式各求值一次。依赖包需显式调用自己公开的挂载函数，导入不会自动注册。
- `[path]` 支持 `Int`/`String`；`[query("tag")]` 的 `String` 要求恰好一次，`List<String>` 保留所有值；`[body]` 使用同模块可见的 `jsonCodec<Type>()`，也可写 `[body("codecFactory")]` 指定零参数工厂。参数不符或 JSON 解码失败回 `400`，不调用业务函数。
- 相同方法、相同路径形状是冲突，参数改名不影响形状。两个宏路由冲突时，编译诊断定位到两个声明。与手写注册冲突时，`Router.mount` 报告两条路由，并且整批都不提交；已经启动的 `serve` 继续用其启动时的路由快照。

HTTP/1.1 默认支持 keep-alive，HTTP/1.0 通过 `Connection: keep-alive` 开启。`request.respond(response)` 返回 `Result<Unit, HttpError>`，成功写完后释放当前请求并复用连接，客户端要求关闭、服务停机或达到单连接请求上限时关闭连接。每条请求独立保留自己的正文、首部与路由，完成后的旧 Request 无法关闭或响应下一条请求。

`HttpServerOptions` 的 `maxBodyBytes` 默认 8 MiB，`maxRequestBytes` 默认 16 MiB，分别限制解码后正文和整条请求的传输字节。目标最多 8 KiB，头部最多 100 项且字段名和值合计最多 32 KiB。`keepAliveTimeoutMs` 默认 5000，控制两条请求之间的空闲时间，设为 0 关闭复用。`maxRequestsPerConnection` 默认 1000。同一连接顺序处理请求，管线输入只保留有界读取窗口。

超限回 `413`，格式或头部文本非法回 `400`。HTTP/1.1 必须有且只有一个有效的 `Host`，携带 `Expect` 的请求在读完首部后回 `417`。响应在写前验证失败可改正后重试。TLS 复用同一套请求和生命周期语义，见 [`std.httpTls`](../httpTls/README.md)。

## 关闭与断线

`Shutdown.request()` 停止接入并丢弃未认领请求，等待已进入的 handler 最多 `options.graceMs` 毫秒（默认 5000）；到期取消剩余任务，再等原生工作和清理结束。直接取消服务则跳过宽限期。`Listener` 和 `Request` 的 `close()` 只请求关闭，可重复调用；`await closeAndWait()` 等待资源回收。并发响应只有一个胜者，另一个得到 `Busy` 或 `Closed`。

```lyra
val shutdown = Shutdown()
val serving = spawn router.serve(listener, shutdown: Some(shutdown))
await waitForTerminationSignal()
shutdown.request()
await serving
```

`Shutdown.request()` 可重复调用；该对象用于在异步任务间传递停机信号，不代表操作系统信号处理器。

`await request.waitDisconnect(timeoutMs)` 在对端断线时返回 `true`，超时、本地关闭或当前响应完成返回 `false`。`disconnected()` 读取已经观测到的断线状态。完整请求后的 TCP 半关闭仍可响应。Router 自动取消对端已断线的 handler 及其子任务，手写接收循环可以用一个子任务等待断线并取消 handler。

连接读写、TLS 握手和断线由独立 Reactor 推进，直接 `await`，无需 `parallel` 或 `blocking`。SQLite 等同步工作仍用 `blocking`；完整的 [SQLite 服务](../../examples/http-sqlite/README.md) 演示了这条路径。

解析器为静态链接的官方 llhttp 9.3.0。开发机首次配置会下载带 SHA-256 校验的**生成版 C 源码**到 `$XDG_CACHE_HOME/lyra/toolchain`（未设置则 `$HOME/.cache/lyra/toolchain`）。以后两个预设及多个工作区共享缓存。离线配置用 `cmake --preset dev -DLYRA_LLHTTP_OFFLINE=ON`；全新离线环境设置 `-DLYRA_LLHTTP_SOURCE_DIR=/path/to/llhttp-release-v9.3.0`。约束使用上游 `release/v9.3.0` 的归档，而不是需要 Node.js 生成 C 的普通源码标签。应用部署不需要 Node.js、动态 llhttp 或编译器。
