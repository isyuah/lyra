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

pub enum HttpClientErrorKind {
    InvalidRequest, TimedOut, TooLarge, TooManyRedirects, RedirectRejected, Tls, Network, Cancelled, Busy, Closed,
}

pub struct HttpClientError(pub val kind: HttpClientErrorKind, pub val code: Int) {
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

pub struct HttpClientOptions(
    pub val connectTimeoutMs: Int = 5000,
    pub val timeoutMs: Int = 30000,
    pub val maxResponseBytes: Int = 8388608,
    pub val maxRedirects: Int = 0,
    pub val proxy: String? = None,
    pub val caPem: Bytes? = None,
)

pub struct HttpHeader(pub val name: String, pub val value: String)
pub struct ClientHeader(pub val name: String, pub val value: Bytes)

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

pub struct ClientResponse(pub val status: Int, pub val headers: FrozenList<ClientHeader>, pub val body: Bytes) {
    pub fn header(name: String): Bytes? { firstHeader(headers, name) }
    pub fn headerValues(name: String): List<Bytes> { matchingHeaders(headers, name) }
    pub fn text(): String? { body.toString() }
}

pub class ClientStream {
    val handle: CPtr
    pub val status: Int
    pub val headers: FrozenList<ClientHeader>

    init(handle: CPtr, status: Int, headers: FrozenList<ClientHeader>) {
        this.handle = handle
        this.status = status
        this.headers = headers
    }

    pub fn header(name: String): Bytes? { firstHeader(headers, name) }
    pub fn headerValues(name: String): List<Bytes> { matchingHeaders(headers, name) }
    pub async fn read(): Result<Bytes?, HttpClientError> {
        val result = await lyraHttpClientReadAsync(handle)
        if lyraHttpClientErrorKind(result) != 0 { return Err(responseError(result)) }
        Ok(if lyraHttpClientHasBody(result) { Some(lyraHttpClientBody(result)) } else { None })
    }
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

pub async fn httpRequest(method: String, url: String, headers: List<HttpHeader> = [], body: Bytes? = None,
                         options: HttpClientOptions = HttpClientOptions()): Result<ClientResponse, HttpClientError> {
    val request = prepareRequest(method, url, headers, body, options)?
    val response = await lyraHttpClientSendAsync(request)
    if lyraHttpClientErrorKind(response) != 0 { return Err(responseError(response)) }
    Ok(ClientResponse(lyraHttpClientStatus(response), responseHeaders(response), lyraHttpClientBody(response)))
}

pub async fn withHttpResponse<T>(method: String, url: String, consume: (ClientStream) -> Task<Result<T, HttpClientError>>,
    headers: List<HttpHeader> = [], body: Bytes? = None, options: HttpClientOptions = HttpClientOptions()): Result<T, HttpClientError> {
    val request = prepareRequest(method, url, headers, body, options)?
    val response = await lyraHttpClientOpenAsync(request)
    if lyraHttpClientErrorKind(response) != 0 { return Err(responseError(response)) }
    await consume(ClientStream(response, lyraHttpClientStatus(response), responseHeaders(response)))
}

pub fn httpGet(url: String, options: HttpClientOptions = HttpClientOptions()): Task<Result<ClientResponse, HttpClientError>> {
    httpRequest("GET", url, options: options)
}
