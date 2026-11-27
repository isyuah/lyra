extern fn lyraHttpClientPrepare(method: String, url: String, body: Bytes, proxy: String, caPem: Bytes,
    connectTimeoutMs: Int, timeoutMs: Int, maxResponseBytes: Int, maxRedirects: Int,
    hasBody: Bool, hasProxy: Bool, hasCa: Bool, count: Int): CPtr;
extern fn lyraHttpClientSetHeader(request: CPtr, index: Int, name: String, value: String): Unit;
extern fn lyraHttpClientSendAsync(request: CPtr): Task<CPtr>;
extern fn lyraHttpClientOpenAsync(request: CPtr): Task<CPtr>;
extern fn lyraHttpClientReadAsync(response: CPtr): Task<CPtr>;
extern fn lyraHttpClientCloseAsync(response: CPtr): Task<CPtr>;
extern fn lyraHttpClientHasBody(response: CPtr): Bool;
extern fn lyraHttpClientErrorKind(response: CPtr): Int;
extern fn lyraHttpClientErrorCode(response: CPtr): Int;
extern fn lyraHttpClientStatus(response: CPtr): Int;
extern fn lyraHttpClientHeaderCount(response: CPtr): Int;
extern fn lyraHttpClientHeaderName(response: CPtr, index: Int): String;
extern fn lyraHttpClientHeaderValue(response: CPtr, index: Int): Bytes;
extern fn lyraHttpClientBody(response: CPtr): Bytes;
extern fn lyraHttpClientHeaderMatches(first: String, second: String): Bool;

/// 客户端请求或读取流的失败类别；HTTP `4xx` 和 `5xx` 本身仍是正常响应。
pub enum HttpClientErrorKind {
    /// 方法、URL、首部、正文或请求选项不合法。
    InvalidRequest,
    /// 连接、请求或响应读取超过时限。
    TimedOut,
    /// 响应首部和正文用完了累计字节预算。
    TooLarge,
    /// 已跟随的跳转次数达到允许上限。
    TooManyRedirects,
    /// 跳转不符合客户端的重定向策略，例如目标不再同源。
    RedirectRejected,
    /// TLS 连接或证书、主机名校验失败。
    Tls,
    /// 底层网络传输失败，可读取原生错误码。
    Network,
    /// 底层传输被取消；调用方的任务取消仍遵循 `Task` 的取消规则。
    Cancelled,
    /// 请求容量耗尽，或同一响应流已有挂起读取。
    Busy,
    /// 流已关闭或已经离开消费作用域。
    Closed,
}

/// 结构化的客户端操作错误，不携带目标或代理 URL。
pub struct HttpClientError(
    /// 可用于区分重试、配置错误或资源限制的分类。
    pub val kind: HttpClientErrorKind,
    /// 原生传输错误码；没有原生错误码时可以为 `0`。
    pub val code: Int
) {
    /// 生成人类可读的错误详情，不打印请求地址。
    pub fn message(): String {
        match kind {
            InvalidRequest => "invalid HTTP method, URL, header, body, or request option",
            TimedOut => "HTTP request timed out",
            TooLarge => "HTTP response exceeded its byte limit",
            TooManyRedirects => "HTTP request exceeded its redirect limit",
            RedirectRejected => "HTTP redirect rejected",
            Tls => "HTTP TLS connection or certificate verification failed",
            Network => "HTTP transfer failed (native code ${code})",
            Cancelled => "HTTP request cancelled",
            Busy => "HTTP client request capacity reached",
            Closed => "HTTP response stream is closed",
        }
    }
}

/// 控制连接、总时限、重定向和 TLS 信任的客户端配置。
pub struct HttpClientOptions(
    /// 建立连接的毫秒上限，须为正数。
    pub val connectTimeoutMs: Int = 5000,
    /// 整次请求的毫秒上限，包含排队、跳转和流式消费暂停的时间。
    pub val timeoutMs: Int = 30000,
    /// 整次请求允许的累计响应字节数，包含首部和正文。
    pub val maxResponseBytes: Int = 8388608,
    /// 默认 `0` 不跟随跳转；可设为 `1`–`20` 让 GET/HEAD 跟随同源跳转。
    pub val maxRedirects: Int = 0,
    /// 可选 HTTP/HTTPS 代理地址；未设置时不从环境变量自动获取代理。
    pub val proxy: String? = None,
    /// 可选的 PEM 信任根；未设置时用系统信任根，仍验证主机名。
    pub val caPem: Bytes? = None,
)

/// 客户端发送的一项文本请求首部。
pub struct HttpHeader(
    /// 字段名；由传输层管理的字段不能通过这里覆盖。
    pub val name: String,
    /// 字段的文本值，发送时进行首部格式校验。
    pub val value: String
)

/// 客户端收到的一项响应首部，值以字节保留。
pub struct ClientHeader(
    /// 字段名，查询时按 ASCII 大小写无关匹配。
    pub val name: String,
    /// 保留原始内容的字段值，不要求是 UTF-8。
    pub val value: Bytes
)

fn firstHeader(headers: FrozenList<ClientHeader>, name: String): Bytes? {
    for field in headers {
        if lyraHttpClientHeaderMatches(field.name, name) { return Some(field.value) }
    }
    None
}

fn matchingHeaders(headers: FrozenList<ClientHeader>, name: String): List<Bytes> {
    val values: List<Bytes> = []
    for field in headers {
        if lyraHttpClientHeaderMatches(field.name, name) { values.add(field.value) }
    }
    values
}

/// 已完整接收的响应快照，不需要继续读取连接。
pub struct ClientResponse(
    /// 最终响应的 HTTP 状态码。
    pub val status: Int,
    /// 按收到的顺序保存首部，包括同名的重复字段。
    pub val headers: FrozenList<ClientHeader>,
    /// 已接收的完整正文，不做文本解码。
    pub val body: Bytes
) {
    /// 按 ASCII 大小写无关匹配返回首个同名值；未出现时返回 `None`。
    pub fn header(name: String): Bytes? { firstHeader(headers, name) }
    /// 按响应中的顺序返回所有同名值，不把它们合并。
    pub fn headerValues(name: String): List<Bytes> { matchingHeaders(headers, name) }
    /// 用 UTF-8 解码正文；非法字节返回 `None`，空正文是 `Some("")`。
    pub fn text(): String? { body.toString() }
}

/// 只在 `withHttpResponse` 消费回调内有效的响应流，按需读取正文。
pub class ClientStream {
    val handle: CPtr
    /// 在打开流时已收到的最终 HTTP 状态码。
    pub val status: Int
    /// 最终响应的只读首部快照，保留出现顺序和重复字段。
    pub val headers: FrozenList<ClientHeader>

    init(handle: CPtr, status: Int, headers: FrozenList<ClientHeader>) {
        this.handle = handle
        this.status = status
        this.headers = headers
    }

    /// 按 ASCII 大小写无关匹配返回首个同名首部值。
    pub fn header(name: String): Bytes? { firstHeader(headers, name) }
    /// 按出现顺序返回所有同名值，不消费正文。
    pub fn headerValues(name: String): List<Bytes> { matchingHeaders(headers, name) }
    /// 等待下一字节块；正常结束为 `Ok(None)`。同时只允许一个挂起读取，取消会关闭传输。
    pub async fn read(): Result<Bytes?, HttpClientError> {
        val result = await lyraHttpClientReadAsync(handle)
        if lyraHttpClientErrorKind(result) != 0 { return Err(responseError(result)) }
        Ok(if lyraHttpClientHasBody(result) { Some(lyraHttpClientBody(result)) } else { None })
    }
    /// 显式关闭并等待传输清理；挂起和后续读取会报告 `Closed`。
    pub async fn close(): Unit { val _ = await lyraHttpClientCloseAsync(handle) }
}

fn responseHeaders(response: CPtr): FrozenList<ClientHeader> {
    val headers: List<ClientHeader> = []
    for index in 0..<lyraHttpClientHeaderCount(response) {
        headers.add(ClientHeader(lyraHttpClientHeaderName(response, index), lyraHttpClientHeaderValue(response, index)))
    }
    FrozenList(headers)
}

fn responseError(response: CPtr): HttpClientError {
    val status = lyraHttpClientErrorKind(response)
    val kind = match status {
        1 => HttpClientErrorKind.InvalidRequest,
        2 => HttpClientErrorKind.TimedOut,
        3 => HttpClientErrorKind.TooLarge,
        4 => HttpClientErrorKind.TooManyRedirects,
        5 => HttpClientErrorKind.RedirectRejected,
        6 => HttpClientErrorKind.Tls,
        7 => HttpClientErrorKind.Network,
        8 => HttpClientErrorKind.Cancelled,
        9 => HttpClientErrorKind.Busy,
        10 => HttpClientErrorKind.Closed,
        _ => throw Exception("unexpected native HTTP client status"),
    }
    HttpClientError(kind, lyraHttpClientErrorCode(response))
}

fn prepareRequest(method: String, url: String, headers: List<HttpHeader>, body: Bytes?, options: HttpClientOptions): Result<CPtr, HttpClientError> {
    if options.timeoutMs < 1 || options.timeoutMs > 2147483647 {
        return Err(HttpClientError(HttpClientErrorKind.InvalidRequest, 0))
    }
    val empty = "".toBytes()
    val snapshot = FrozenList(headers)
    val count = snapshot.size
    val request = lyraHttpClientPrepare(method, url, body ?? empty, options.proxy ?? "", options.caPem ?? empty,
        options.connectTimeoutMs, options.timeoutMs, options.maxResponseBytes, options.maxRedirects,
        body != None, options.proxy != None, options.caPem != None, count)
    for index in 0..<count {
        val header = snapshot[index]
        lyraHttpClientSetHeader(request, index, header.name, header.value)
    }
    Ok(request)
}

/// 启动时向绝对 HTTP/HTTPS URL 发送请求并读完响应；`GET` 和 `HEAD` 不接受请求体。
/// HTTP 错误状态码仍返回响应，连接、校验或传输失败才返回 `HttpClientError`。
pub async fn httpRequest(method: String, url: String, headers: List<HttpHeader> = [], body: Bytes? = None,
                         options: HttpClientOptions = HttpClientOptions()): Result<ClientResponse, HttpClientError> {
    val request = prepareRequest(method, url, headers, body, options)?
    val response = await lyraHttpClientSendAsync(request)
    if lyraHttpClientErrorKind(response) != 0 { return Err(responseError(response)) }
    Ok(ClientResponse(lyraHttpClientStatus(response), responseHeaders(response), lyraHttpClientBody(response)))
}

/// 收到最终响应头后调用 `consume`。回调退出时等待连接交接或关闭，未完成传输不会逃出作用域。
pub async fn withHttpResponse<T>(method: String, url: String, consume: (ClientStream) -> Task<Result<T, HttpClientError>>,
    headers: List<HttpHeader> = [], body: Bytes? = None, options: HttpClientOptions = HttpClientOptions()): Result<T, HttpClientError> {
    val request = prepareRequest(method, url, headers, body, options)?
    val response = await lyraHttpClientOpenAsync(request)
    if lyraHttpClientErrorKind(response) != 0 { return Err(responseError(response)) }
    await consume(ClientStream(response, lyraHttpClientStatus(response), responseHeaders(response)))
}

/// 构造不带正文的冷 GET 请求，完成时返回已缓冲的完整响应。
pub fn httpGet(url: String, options: HttpClientOptions = HttpClientOptions()): Task<Result<ClientResponse, HttpClientError>> {
    httpRequest("GET", url, options: options)
}
