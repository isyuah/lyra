extern fn lyraHttpClientPrepare(method: String, url: String, body: Bytes, proxy: String, caPem: Bytes,
    connectTimeoutMs: Int, timeoutMs: Int, maxResponseBytes: Int, maxRedirects: Int,
    hasBody: Bool, hasProxy: Bool, hasCa: Bool, count: Int): CPtr;
extern fn lyraHttpClientSetHeader(request: CPtr, index: Int, name: String, value: String): Unit;
extern fn lyraHttpClientSend(request: CPtr): CPtr;
extern fn lyraHttpClientErrorKind(response: CPtr): Int;
extern fn lyraHttpClientErrorCode(response: CPtr): Int;
extern fn lyraHttpClientStatus(response: CPtr): Int;
extern fn lyraHttpClientHeaderCount(response: CPtr): Int;
extern fn lyraHttpClientHeaderName(response: CPtr, index: Int): String;
extern fn lyraHttpClientHeaderValue(response: CPtr, index: Int): Bytes;
extern fn lyraHttpClientBody(response: CPtr): Bytes;
extern fn lyraHttpClientHeaderMatches(first: String, second: String): Bool;

pub enum HttpClientErrorKind {
    InvalidRequest, TimedOut, TooLarge, TooManyRedirects, RedirectRejected, Tls, Network, Cancelled,
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

pub struct ClientResponse(pub val status: Int, pub val headers: FrozenList<ClientHeader>, pub val body: Bytes) {
    pub fn header(name: String): Bytes? {
        for field in headers {
            if lyraHttpClientHeaderMatches(field.name, name) { return Some(field.value) }
        }
        None
    }

    pub fn headerValues(name: String): List<Bytes> {
        val values: List<Bytes> = []
        for field in headers {
            if lyraHttpClientHeaderMatches(field.name, name) { values.add(field.value) }
        }
        values
    }

    pub fn text(): String? { body.toString() }
}

fn unpack(response: CPtr): Result<ClientResponse, HttpClientError> {
    val status = lyraHttpClientErrorKind(response)
    if status == 0 {
        val headers: List<ClientHeader> = []
        for index in 0..<lyraHttpClientHeaderCount(response) {
            headers.add(ClientHeader(lyraHttpClientHeaderName(response, index), lyraHttpClientHeaderValue(response, index)))
        }
        return Ok(ClientResponse(lyraHttpClientStatus(response), FrozenList(headers), lyraHttpClientBody(response)))
    }
    val kind = match status {
        1 => HttpClientErrorKind.InvalidRequest,
        2 => HttpClientErrorKind.TimedOut,
        3 => HttpClientErrorKind.TooLarge,
        4 => HttpClientErrorKind.TooManyRedirects,
        5 => HttpClientErrorKind.RedirectRejected,
        6 => HttpClientErrorKind.Tls,
        7 => HttpClientErrorKind.Network,
        8 => HttpClientErrorKind.Cancelled,
        _ => throw Exception("unexpected native HTTP client status"),
    }
    Err(HttpClientError(kind, lyraHttpClientErrorCode(response)))
}

pub async fn httpRequest(method: String, url: String, headers: List<HttpHeader> = [], body: Bytes? = None,
                         options: HttpClientOptions = HttpClientOptions()): Result<ClientResponse, HttpClientError> {
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
    val results = Channel<Result<ClientResponse, HttpClientError>>(1)
    val worker = spawn async {
        val response = await blocking { unpack(lyraHttpClientSend(request)) }
        await results.send(response)
    }
    await select {
        val received = results.receive() => received ?? throw Exception("HTTP response channel closed unexpectedly"),
        sleepMs(options.timeoutMs) => {
            worker.cancel()
            val _ = awaitOutcome(worker)
            Err(HttpClientError(HttpClientErrorKind.TimedOut, 0))
        },
    }
}

pub fn httpGet(url: String, options: HttpClientOptions = HttpClientOptions()): Task<Result<ClientResponse, HttpClientError>> {
    httpRequest("GET", url, options: options)
}
