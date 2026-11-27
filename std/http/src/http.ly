import std.io.{writeError}
import std.json.{JsonCodec}
import std.sync.{Locked}
import std.task.{withTimeout}

extern fn lyraHttpListen(host: String, port: Int, options: CPtr): CPtr;
extern fn lyraHttpOptions(capacity: Int, requestTimeoutMs: Int, idleReadTimeoutMs: Int, writeTimeoutMs: Int, handshakeTimeoutMs: Int, graceMs: Int, claimTimeoutMs: Int, maxBodyBytes: Int, maxRequestBytes: Int, keepAliveTimeoutMs: Int, maxRequestsPerConnection: Int, streamRequestBody: Bool, enableHttp2: Bool): CPtr;
extern fn lyraHttpStatus(server: CPtr): Int;
extern fn lyraHttpStatusDetail(server: CPtr): String;
extern fn lyraHttpErrorKind(status: Int): Int;
extern fn lyraHttpPort(server: CPtr): Int;
extern fn lyraHttpCapacity(server: CPtr): Int;
extern fn lyraHttpGraceMs(server: CPtr): Int;
extern fn lyraHttpBeginDrain(server: CPtr): Unit;
extern fn lyraHttpStop(server: CPtr): Unit;
extern fn lyraHttpListenerCloseAsync(server: CPtr): Task<Unit>;
extern fn lyraHttpListenerScopeAsync(server: CPtr): Task<Unit>;
extern fn lyraHttpKeepListener(server: CPtr): Unit;
extern fn lyraHttpAcceptAsync(server: CPtr, timeoutMs: Int, scoped: Bool, reportRejections: Bool): Task<CPtr>;
extern fn lyraHttpRequestStatus(request: CPtr): Int;
extern fn lyraHttpMethod(request: CPtr): String;
extern fn lyraHttpTarget(request: CPtr): String;
extern fn lyraHttpBody(request: CPtr): Bytes;
extern fn lyraHttpBodyStreaming(request: CPtr): Bool;
extern fn lyraHttpReadBodyAsync(request: CPtr): Task<CPtr>;
extern fn lyraHttpBodyReadStatus(result: CPtr): Int;
extern fn lyraHttpBodyReadHasBytes(result: CPtr): Bool;
extern fn lyraHttpBodyReadBytes(result: CPtr): Bytes;
extern fn lyraHttpDisconnected(request: CPtr): Bool;
extern fn lyraHttpWaitDisconnectAsync(request: CPtr, timeoutMs: Int): Task<Int>;
extern fn lyraHttpClaim(request: CPtr): Int;
extern fn lyraHttpDrop(request: CPtr): Unit;
extern fn lyraHttpRequestCloseAsync(request: CPtr): Task<Unit>;
extern fn lyraHttpResponseHeaders(count: Int): CPtr;
extern fn lyraHttpResponseHeaderSet(headers: CPtr, index: Int, name: String, value: String): Int;
extern fn lyraHttpRespondAsync(request: CPtr, code: Int, mediaType: String, body: Bytes, headers: CPtr): Task<Int>;
extern fn lyraHttpStartStreamAsync(request: CPtr, code: Int, mediaType: String, headers: CPtr): Task<Int>;
extern fn lyraHttpWriteStreamAsync(request: CPtr, body: Bytes, final: Bool): Task<Int>;
extern fn lyraHttpRouteMethodValid(method: String): Bool;
extern fn lyraHttpRoutePattern(route: String): CPtr;
extern fn lyraHttpRouteValid(route: CPtr): Bool;
extern fn lyraHttpRouteSameShape(left: CPtr, right: CPtr): Bool;
extern fn lyraHttpRouteMoreSpecific(left: CPtr, right: CPtr): Bool;
extern fn lyraHttpRouteMatch(request: CPtr, route: CPtr): Bool;
extern fn lyraHttpRouteBind(request: CPtr, route: CPtr): Unit;
extern fn lyraHttpPathIndex(request: CPtr, name: String): Int;
extern fn lyraHttpPathValue(request: CPtr, index: Int): String;
extern fn lyraHttpParseInt(value: String): CPtr;
extern fn lyraHttpIntegerValid(parsed: CPtr): Bool;
extern fn lyraHttpIntegerValue(parsed: CPtr): Int;
extern fn lyraHttpQueryCount(request: CPtr): Int;
extern fn lyraHttpQueryName(request: CPtr, index: Int): String;
extern fn lyraHttpQueryValue(request: CPtr, index: Int): String;
extern fn lyraHttpHeaderCount(request: CPtr): Int;
extern fn lyraHttpHeaderMatches(request: CPtr, index: Int, name: String): Bool;
extern fn lyraHttpHeaderValue(request: CPtr, index: Int): String;
extern fn lyraHttpCookie(request: CPtr, name: String): CPtr;
extern fn lyraHttpCookieValue(value: CPtr): String;

/// 服务端监听、传输、解析和请求读取的失败类别。
pub enum HttpErrorKind {
    /// 原生网络操作失败，载荷为系统错误码。
    System(Int),
    /// 入参、取值格式或当前请求模式不符合操作要求。
    Invalid,
    /// 监听器、连接或请求已关闭。
    Closed,
    /// 同一资源正被不兼容的并发操作使用。
    Busy,
    /// 等待请求、读取或写出超过相应时限。
    Timeout,
    /// HTTP 报文不符合协议格式。
    Malformed,
    /// 报文或正文超出配置的体积上限。
    TooLarge,
    /// 需要文本的字段或正文不是有效的 UTF-8。
    InvalidUtf8,
    /// JSON 正文解析失败或不符合指定 codec。
    InvalidJson,
    /// 单值查询遇到同名字段的多次出现；用 `headers` 读取多值。
    DuplicateHeader,
    /// TLS 证书、密钥或监听配置无效。
    TlsConfig,
    /// TLS 握手未成功完成。
    HandshakeFailed,
    /// TLS 握手超过独立限时。
    HandshakeTimeout,
    /// 收到了服务端不支持的 `Expect` 请求头。
    UnsupportedExpectation,
}

/// 服务端操作失败，保留操作名、分类和可选详情。
pub struct HttpError(
    /// 用于决定如何处理失败的错误分类。
    pub val kind: HttpErrorKind,
    /// 失败的操作，如 `accept` 或 `request.readBody`。
    pub val operation: String,
    /// 具体失败说明；为空时，`message()` 根据分类生成说明。
    pub val detail: String = ""
) {
    /// 返回带操作名前缀的可读错误，优先采用 `detail`。
    pub fn message(): String {
        if detail != "" { return operation + ": " + detail }
        operation + ": " + match kind {
            System(code) => "system network error (code ${code})",
            Invalid => "invalid HTTP argument",
            Closed => "HTTP connection or listener is closed",
            Busy => "HTTP connection or listener is in use",
            Timeout => "HTTP operation timed out",
            Malformed => "malformed HTTP message",
            TooLarge => "HTTP message exceeds its size limit",
            InvalidUtf8 => "HTTP text is not UTF-8",
            InvalidJson => "invalid JSON body",
            DuplicateHeader => "HTTP header has multiple values",
            TlsConfig => "invalid TLS configuration",
            HandshakeFailed => "TLS handshake failed",
            HandshakeTimeout => "TLS handshake timed out",
            UnsupportedExpectation => "unsupported HTTP expectation",
        }
    }
}

fn httpError(operation: String, status: Int, detail: String = ""): HttpError {
    val kind = match lyraHttpErrorKind(status) {
        1 => HttpErrorKind.Invalid,
        2 => HttpErrorKind.Closed,
        3 => HttpErrorKind.Timeout,
        4 => HttpErrorKind.Malformed,
        5 => HttpErrorKind.TooLarge,
        6 => HttpErrorKind.InvalidUtf8,
        7 => HttpErrorKind.TlsConfig,
        8 => HttpErrorKind.HandshakeFailed,
        9 => HttpErrorKind.HandshakeTimeout,
        10 => HttpErrorKind.Busy,
        11 => HttpErrorKind.UnsupportedExpectation,
        _ => HttpErrorKind.System(status),
    }
    HttpError(kind, operation, detail)
}

/// 明文与 TLS 监听器共享的并发、体积和超时限制。
pub struct HttpServerOptions(
    /// 同时运行的请求处理器上限，范围为 `1` 到 `64`。
    pub val capacity: Int = 4,
    /// 接入后读取完整请求的总毫秒预算；复用连接从收到新请求的首个字节计时。
    pub val requestTimeoutMs: Int = 30000,
    /// 等待下一个 HTTP 输入字节的空闲上限（毫秒）；`None` 表示不设单独的空闲上限。
    pub val idleReadTimeoutMs: Int? = None,
    /// 从响应开始写出到传输完成的毫秒预算，包含流式正文。
    pub val writeTimeoutMs: Int = 5000,
    /// TLS 握手的独立毫秒预算，不占正文读取预算。
    pub val handshakeTimeoutMs: Int = 5000,
    /// 优雅停机等待已进入的处理器完成的毫秒数，到期后请求取消。
    pub val graceMs: Int = 5000,
    /// 裸 `accept` 返回的请求可以等待被认领的毫秒数。
    pub val claimTimeoutMs: Int = 5000,
    /// 请求正文去掉 HTTP 传输分块后允许的总字节数，不解压内容编码。
    pub val maxBodyBytes: Int = 8388608,
    /// 单个请求允许的传输字节总量，包括首部和正文。
    pub val maxRequestBytes: Int = 16777216,
    /// 复用连接在请求间的空闲毫秒数，`0` 禁止复用。
    pub val keepAliveTimeoutMs: Int = 5000,
    /// 一条连接最多处理的请求数，达上限后不再接受新请求。
    pub val maxRequestsPerConnection: Int = 1000,
    /// 首部就绪后即可处理请求，并通过 `readBody()` 逐块读取正文；默认先缓冲完整正文。
    pub val streamRequestBody: Bool = false,
    /// TLS 监听器用 ALPN 协商 HTTP/2；关闭后仅协商 HTTP/1.1。
    pub val enableHttp2: Bool = true,
)

fn serverOptions(options: HttpServerOptions): CPtr {
    val idle = match options.idleReadTimeoutMs {
        None => -1,
        Some(milliseconds) => if milliseconds < 1 { 0 } else { milliseconds },
    }
    lyraHttpOptions(options.capacity, options.requestTimeoutMs, idle, options.writeTimeoutMs, options.handshakeTimeoutMs, options.graceMs, options.claimTimeoutMs, options.maxBodyBytes, options.maxRequestBytes, options.keepAliveTimeoutMs, options.maxRequestsPerConnection, options.streamRequestBody, options.enableHttp2)
}

fn acceptResult(request: CPtr): Result<Request, HttpError> {
    val status = lyraHttpRequestStatus(request)
    if status == 0 { Ok(Request(request)) } else { Err(httpError("accept", status)) }
}

/// 已绑定的 HTTP 或 HTTPS 监听器，供手动接收请求或交给 `Router.serve`。
pub class Listener {
    val handle: CPtr
    init(handle: CPtr) { this.handle = handle }

    /// 返回实际绑定的端口，亦可用于查询由 `port: 0` 分配的端口。
    pub fn port(): Int { lyraHttpPort(handle) }
    /// 等待就绪请求；`-1` 无期限，`0` 只取当前就绪项。返回裸请求，调用方负责关闭。
    pub async fn accept(timeoutMs: Int = -1): Result<Request, HttpError> {
        acceptResult(await lyraHttpAcceptAsync(handle, timeoutMs, false, true))
    }
    /// 等待并立即认领一个请求，运行回调并在其任务作用域退出后等待请求清理。
    pub async fn withAccepted<T>(timeoutMs: Int, handleRequest: (Request) -> Task<T>): Result<T, HttpError> {
        val request = acceptResult(await lyraHttpAcceptAsync(handle, timeoutMs, true, true))?
        Ok(await handleRequest(request))
    }
    /// 请求停止监听，可重复调用；需要等待资源回收时用 `closeAndWait()`。
    pub fn close(): Unit { lyraHttpStop(handle) }
    /// 请求停止监听并等待原生资源回收。
    pub async fn closeAndWait(): Unit { await lyraHttpListenerCloseAsync(handle) }
}

/// 一项文本 HTTP 首部；同名字段可以分项保存。
pub struct Header(
    /// HTTP 字段名，匹配时不区分 ASCII 大小写。
    pub val name: String,
    /// 字段值，不包含名称或冒号。
    pub val value: String
)

/// 流式响应正文的写入端，应在生产函数作用域内按顺序等待每次写入。
pub class ResponseWriter {
    val sink: (Bytes) -> Task<Result<Unit, HttpError>>
    init(sink: (Bytes) -> Task<Result<Unit, HttpError>>) { this.sink = sink }

    /// 用自定义异步接收端构造写入器，适合在传输前串接正文转换。
    pub static fn fromSink(sink: (Bytes) -> Task<Result<Unit, HttpError>>): ResponseWriter {
        ResponseWriter(sink)
    }

    /// 传递一个原始字节块并等待下游写入。网络写入并发竞争返回 `Busy`，空块不写到线上。
    pub async fn write(content: Bytes): Result<Unit, HttpError> { await sink(content) }
}

async fn writeResponseChunk(handle: CPtr, content: Bytes): Result<Unit, HttpError> {
    val status = await lyraHttpWriteStreamAsync(handle, content, false)
    if status == 0 { Ok({}) } else { Err(httpError("response.write", status)) }
}

/// 响应正文可以是已备好的字节，也可以按需生产。
pub enum ResponseBody {
    /// 要发送的完整正文，已在内存中准备好。
    Buffered(Bytes),
    /// 首部写出后才调用的正文生产函数；`HEAD` 不执行它。
    Streaming((ResponseWriter) -> Task<Result<Unit, HttpError>>),
}

/// 等待写出的 HTTP 响应；首部与状态会在写出时校验，不会在构造时发送。
pub struct Response(
    /// HTTP 状态码。
    pub val code: Int,
    /// 正文的媒体类型，由传输层写为 `Content-Type`。
    pub val contentType: String,
    /// 预缓冲正文或随后运行的流式生产函数。
    pub val body: ResponseBody,
    /// 应用附加的只读首部列表；长度与连接管理字段由传输层维护。
    pub val headers: FrozenList<Header> = FrozenList<Header>([])
) {
    /// 以 UTF-8 字符串构造 `text/plain` 响应。
    pub static fn text(code: Int, body: String): Response { Response.bytes(code, "text/plain", body.toBytes()) }
    /// 使用指定媒体类型与完整字节正文构造响应，不做字符解码。
    pub static fn bytes(code: Int, contentType: String, body: Bytes): Response { Response(code, contentType, ResponseBody.Buffered(body)) }
    /// 构造由回调分块输出的响应；头部先发送，`HEAD` 跳过回调。
    pub static fn stream(code: Int, contentType: String, produce: (ResponseWriter) -> Task<Result<Unit, HttpError>>): Response {
        Response(code, contentType, ResponseBody.Streaming(produce))
    }
    /// 立即用指定 codec 编码正文，媒体类型为 `application/json`，状态默认 `200`。
    pub static fn json<T>(value: T, codec: JsonCodec<T>, code: Int = 200): Response {
        Response.bytes(code, "application/json", codec.encode(value).toBytes())
    }
    /// 返回追加一项首部的新响应，不覆盖旧值，也不修改原响应。
    pub fn withHeader(name: String, value: String): Response {
        val fields: List<Header> = []
        for field in headers { fields.add(field) }
        fields.add(Header(name, value))
        Response(code, contentType, body, FrozenList(fields))
    }
}

/// 一个服务端请求，包含路由绑定、首部、正文和响应生命周期。
pub class Request {
    val handle: CPtr
    init(handle: CPtr) { this.handle = handle }

    /// 返回客户端发送的 HTTP 方法。
    pub fn method(): String { lyraHttpMethod(handle) }
    /// 返回原始请求目标，保留查询串。
    pub fn target(): String { lyraHttpTarget(handle) }
    /// 判断正文是否配置为按需流式读取。
    pub fn bodyStreaming(): Bool { lyraHttpBodyStreaming(handle) }
    /// 返回缓冲模式的完整原始正文；流式模式应调用 `readBody()`，否则抛异常。
    pub fn body(): Bytes {
        if bodyStreaming() { throw Exception("request.body is buffered-only; use await request.readBody()") }
        lyraHttpBody(handle)
    }
    /// 异步读取下一块正文，正常结束为 `Ok(None)`；同一请求的并发读取返回 `Busy`。
    pub async fn readBody(): Result<Bytes?, HttpError> {
        val result = await lyraHttpReadBodyAsync(handle)
        val status = lyraHttpBodyReadStatus(result)
        if status != 0 { return Err(httpError("request.readBody", status)) }
        Ok(if lyraHttpBodyReadHasBytes(result) { Some(lyraHttpBodyReadBytes(result)) } else { None })
    }
    /// 将缓冲正文按 UTF-8 解码；非法字节返回 `InvalidUtf8`，流式正文返回 `Invalid`。
    pub fn text(): Result<String, HttpError> {
        if bodyStreaming() { return Err(HttpError(HttpErrorKind.Invalid, "request.text", "use await request.readBody()")) }
        match body().toString() {
            Some(text) => Ok(text),
            None => Err(HttpError(HttpErrorKind.InvalidUtf8, "request.text")),
        }
    }
    /// 按名称读取路由模板绑定的路径参数；未绑定返回 `None`。
    pub fn path(name: String): String? {
        val index = lyraHttpPathIndex(handle, name)
        if index < 0 { None } else { Some(lyraHttpPathValue(handle, index)) }
    }
    /// 将路径参数解析为 `Int`；未绑定为 `Ok(None)`，非法或越界值返回 `Invalid`。
    pub fn pathInt(name: String): Result<Int?, HttpError> {
        val index = lyraHttpPathIndex(handle, name)
        if index < 0 { return Ok(None) }
        val parsed = lyraHttpParseInt(lyraHttpPathValue(handle, index))
        if lyraHttpIntegerValid(parsed) { Ok(Some(lyraHttpIntegerValue(parsed))) }
        else { Err(HttpError(HttpErrorKind.Invalid, "request.pathInt", "invalid integer path parameter: " + name)) }
    }
    /// 返回同名查询参数的所有值，保留重复项及出现顺序。
    pub fn queryAll(name: String): List<String> {
        val values: List<String> = []
        for index in 0..<lyraHttpQueryCount(handle) {
            if lyraHttpQueryName(handle, index) == name { values.add(lyraHttpQueryValue(handle, index)) }
        }
        values
    }
    /// 按 ASCII 大小写无关匹配单值首部；缺失为 `None`，出现多次为 `DuplicateHeader`。
    pub fn header(name: String): Result<String?, HttpError> {
        var value: String? = None
        for index in 0..<lyraHttpHeaderCount(handle) {
            if lyraHttpHeaderMatches(handle, index, name) {
                if value != None {
                    return Err(HttpError(HttpErrorKind.DuplicateHeader, "request.header", "multiple values for header: " + name))
                }
                value = Some(lyraHttpHeaderValue(handle, index))
            }
        }
        Ok(value)
    }
    /// 不区分 ASCII 大小写地读取该首部的全部值，按出现顺序返回。
    pub fn headers(name: String): List<String> {
        val values: List<String> = []
        for index in 0..<lyraHttpHeaderCount(handle) {
            if lyraHttpHeaderMatches(handle, index, name) { values.add(lyraHttpHeaderValue(handle, index)) }
        }
        values
    }
    /// 按区分大小写的 Cookie 名称查找值；找不到时返回 `None`。
    pub fn cookie(name: String): String? {
        val value = lyraHttpCookie(handle, name)
        if value.isNull() { None } else { Some(lyraHttpCookieValue(value)) }
    }
    /// 验证缓冲正文的 UTF-8 并用 codec 解码，JSON 格式或映射失败返回 `InvalidJson`。
    pub fn json<T>(codec: JsonCodec<T>): Result<T, HttpError> {
        val input = text()?
        match codec.decode(input) {
            Ok(value) => Ok(value),
            Err(detail) => Err(HttpError(HttpErrorKind.InvalidJson, "request.json", detail)),
        }
    }
    /// 查询已经观察到的对端断线状态，不等待网络事件。
    pub fn disconnected(): Bool { lyraHttpDisconnected(handle) }
    /// 等待对端断线；断线返回 `true`，超时、本地关闭或响应完成返回 `false`。
    pub async fn waitDisconnect(timeoutMs: Int = -1): Bool {
        val status = await lyraHttpWaitDisconnectAsync(handle, timeoutMs)
        if status < 0 { throw Exception(httpError("request.disconnect", -status).message()) }
        status == 1
    }
    /// 认领裸 `accept` 得到的请求，避免等待认领超时；读流或写响应也会认领。
    pub fn claim(): Result<Unit, HttpError> {
        val status = lyraHttpClaim(handle)
        if status == 0 { Ok({}) } else { Err(httpError("request.claim", status)) }
    }
    /// 校验并发送响应，成功后释放请求；并发响应至多一个成功，写前校验失败时可修正重试。
    pub async fn respond(response: Response): Result<Unit, HttpError> {
        val headers = lyraHttpResponseHeaders(response.headers.size)
        if headers.isNull() { return Err(HttpError(HttpErrorKind.TooLarge, "respond")) }
        for index in 0..<response.headers.size {
            val field = response.headers[index]
            val status = lyraHttpResponseHeaderSet(headers, index, field.name, field.value)
            if status != 0 { return Err(httpError("respond", status)) }
        }
        val status = match response.body {
            Buffered(content) => await lyraHttpRespondAsync(handle, response.code, response.contentType, content, headers),
            Streaming(produce) => {
                val started = await lyraHttpStartStreamAsync(handle, response.code, response.contentType, headers)
                if started != 0 { return Err(httpError("respond", started)) }
                if method() != "HEAD" {
                    val writer = ResponseWriter.fromSink((content: Bytes) => { writeResponseChunk(handle, content) })
                    (await produce(writer))?
                }
                await lyraHttpWriteStreamAsync(handle, "".toBytes(), true)
            },
        }
        if status == 0 { Ok({}) } else { Err(httpError("respond", status)) }
    }
    /// 请求关闭或放弃当前请求，可重复调用，不会关闭同一连接上的下一条请求。
    pub fn close(): Unit { lyraHttpDrop(handle) }
    /// 关闭当前请求并等待原生回收。
    pub async fn closeAndWait(): Unit { await lyraHttpRequestCloseAsync(handle) }
}

fn listenerResult(handle: CPtr, operation: String): Result<Listener, HttpError> {
    val status = lyraHttpStatus(handle)
    if status == 0 { Ok(Listener(handle)) } else { Err(httpError(operation, status, lyraHttpStatusDetail(handle))) }
}

/// 在数值 IPv4/IPv6 地址上创建明文监听器；默认仅回环监听，端口 `0` 让系统分配。
pub fn listen(port: Int, host: String = "127.0.0.1", options: HttpServerOptions = HttpServerOptions()): Result<Listener, HttpError> {
    listenerResult(lyraHttpListen(host, port, serverOptions(options)), "listen")
}

/// 可由 `Router.mount` 批量安装的一项路由定义。
pub struct RouteRegistration(
    /// 区分大小写的 HTTP 方法，如 `GET`。
    pub val method: String,
    /// 路径模板，参数段可写为 `{name}`。
    pub val path: String,
    /// 处理匹配请求并返回响应的异步回调。
    pub val handler: (Request) -> Task<Response>,
    /// 用于路由冲突诊断的注册来源说明。
    pub val origin: String = "manual registration"
)

struct Route(val registration: RouteRegistration, val pattern: CPtr)

fn describeRoute(route: RouteRegistration): String { "${route.method} ${route.path} (${route.origin})" }

/// 异步任务间共享的优雅停机信号，可传给 `Router.serve`。
pub class Shutdown {
    val signal: Channel<Unit> = Channel<Unit>(1)
    /// 通知服务停止接入，给已认领请求留出宽限期；可重复调用。
    pub fn request(): Unit { signal.close() }
    async fn wait(): Unit { val _ = await signal.receive() }
}

/// 按方法与路径分派请求。路径不匹配返回 `404`，仅方法不匹配返回 `405` 和 `Allow`。
pub class Router {
    val routes: Locked<List<Route>> = Locked([])

    /// 注册 `GET` 路由；没有显式 `HEAD` 路由时也可用于处理 HEAD 请求。
    pub fn get(pattern: String, handler: (Request) -> Task<Response>): Unit { route("GET", pattern, handler) }
    /// 为路径模板注册 `POST` 处理器。
    pub fn post(pattern: String, handler: (Request) -> Task<Response>): Unit { route("POST", pattern, handler) }
    /// 为路径模板注册 `PUT` 处理器。
    pub fn put(pattern: String, handler: (Request) -> Task<Response>): Unit { route("PUT", pattern, handler) }
    /// 为路径模板注册 `DELETE` 处理器。
    pub fn delete(pattern: String, handler: (Request) -> Task<Response>): Unit { route("DELETE", pattern, handler) }
    /// 为路径模板注册 `PATCH` 处理器。
    pub fn patch(pattern: String, handler: (Request) -> Task<Response>): Unit { route("PATCH", pattern, handler) }
    /// 为路径模板注册显式 `OPTIONS` 处理器。
    pub fn options(pattern: String, handler: (Request) -> Task<Response>): Unit { route("OPTIONS", pattern, handler) }
    /// 注册优先于 `GET` 回退的 `HEAD` 路由；只发送响应首部。
    pub fn head(pattern: String, handler: (Request) -> Task<Response>): Unit { route("HEAD", pattern, handler) }

    /// 注册一项指定方法和路径模板的路由；同方法相同形状的路径会报告冲突。
    pub fn route(method: String, pattern: String, handler: (Request) -> Task<Response>): Unit {
        mount([RouteRegistration(method, pattern, handler)])
    }

    /// 原子安装一批路由，任一项无效或冲突则整批不提交；已启动的服务继续使用旧快照。
    pub fn mount(registrations: List<RouteRegistration>): Unit {
        if registrations.size == 0 { return }
        routes.update { previous =>
            val next: List<Route> = []
            for route in previous { next.add(route) }
            for registration in registrations {
                if !lyraHttpRouteMethodValid(registration.method) { throw Exception("unsupported HTTP method: " + describeRoute(registration)) }
                val pattern = lyraHttpRoutePattern(registration.path)
                if !lyraHttpRouteValid(pattern) { throw Exception("invalid HTTP route: " + describeRoute(registration)) }
                for existing in next {
                    if existing.registration.method == registration.method && lyraHttpRouteSameShape(pattern, existing.pattern) {
                        throw Exception("conflicting HTTP routes: " + describeRoute(existing.registration) + " and " + describeRoute(registration))
                    }
                }
                next.add(Route(registration, pattern))
            }
            next
        }
    }

    /// 接管监听器，按启动时的路由快照和容量处理请求，断线时取消对应处理任务。
    /// 传入 `Shutdown` 可在宽限期内排空请求；直接取消服务会跳过宽限期。
    pub async fn serve(listener: Listener, shutdown: Shutdown? = None): Unit {
        lyraHttpKeepListener(listener.handle)
        val snapshot = routes.snapshot()
        taskGroup {
            val listening = spawn lyraHttpListenerScopeAsync(listener.handle)
            val slots: List<Task<Unit>> = []
            for slot in 0..<lyraHttpCapacity(listener.handle) { slots.add(spawn serveSlot(listener, snapshot)) }
            if let Some(stop) = shutdown {
                val stopping = spawn drainOnShutdown(listener, stop, slots)
                await joinSlots(slots)
                stopping.cancel()
            } else { await joinSlots(slots) }
            await listening
        };
    }
}

fn allow(methods: List<String>, method: String): Unit {
    for existing in methods { if existing == method { return } }
    methods.add(method)
}

async fn dispatch(request: Request, routes: List<Route>): Response {
    val method = request.method()
    val allowed: List<String> = []
    var selected: Route? = None
    var headFallback: Route? = None
    for route in routes {
        if lyraHttpRouteMatch(request.handle, route.pattern) {
            val registered = route.registration.method
            if method == registered {
                selected = preferRoute(selected, route)
            }
            allow(allowed, registered)
            if registered == "GET" {
                allow(allowed, "HEAD")
                if method == "HEAD" { headFallback = preferRoute(headFallback, route) }
            }
        }
    }
    if let Some(route) = (if selected == None { headFallback } else { selected }) {
        lyraHttpRouteBind(request.handle, route.pattern)
        return await route.registration.handler(request)
    }
    if allowed.size == 0 { return Response.text(404, "not found") }
    var methods = allowed[0]
    for index in 1..<allowed.size { methods += ", " + allowed[index] }
    Response.text(405, "method not allowed").withHeader("Allow", methods)
}

fn preferRoute(current: Route?, candidate: Route): Route? {
    if let Some(previous) = current {
        if !lyraHttpRouteMoreSpecific(candidate.pattern, previous.pattern) { return current }
    }
    Some(candidate)
}

async fn respond(request: Request, routes: List<Route>): Unit {
    val response = try { await dispatch(request, routes) } catch {
        val _ = writeError("HTTP handler raised an exception\n")
        Response.text(500, "internal server error")
    }
    if request.disconnected() { return }
    val sent = try { await request.respond(response) } catch {
        val _ = writeError("HTTP response producer raised an exception\n")
        return
    }
    if let Err(error) = sent {
        val _ = writeError("HTTP response failed: ${error.message()}\n")
        if error.kind == HttpErrorKind.Invalid || error.kind == HttpErrorKind.TooLarge {
            val _ = await request.respond(Response.text(500, "internal server error"))
        }
    }
}

async fn cancelOnDisconnect(request: Request, handler: Task<Unit>): Unit {
    if await request.waitDisconnect() { handler.cancel() }
}

async fn serveRequest(listener: Listener, routes: List<Route>): Bool {
    val request = match acceptResult(await lyraHttpAcceptAsync(listener.handle, -1, true, false)) {
        Ok(value) => value,
        Err(error) => {
            if error.kind == HttpErrorKind.Closed { return false }
            throw Exception(error.message())
        }
    }
    taskGroup {
        val handler = spawn respond(request, routes)
        val watcher = spawn cancelOnDisconnect(request, handler)
        if let Failed(error) = awaitOutcome(handler) { throw error }
        watcher.cancel()
    };
    true
}

async fn serveSlot(listener: Listener, routes: List<Route>): Unit {
    while await serveRequest(listener, routes) {}
}

async fn drainOnShutdown(listener: Listener, stop: Shutdown, slots: List<Task<Unit>>): Unit {
    await stop.wait()
    lyraHttpBeginDrain(listener.handle)
    if let Cancelled(_) = awaitOutcome(withTimeout(lyraHttpGraceMs(listener.handle), joinSlots(slots))) {
        lyraHttpStop(listener.handle)
        for slot in slots { slot.cancel() }
    }
}

async fn joinSlots(slots: List<Task<Unit>>): Unit {
    for slot in slots { val _ = awaitOutcome(slot) }
}
