import std.io.{writeError}
import std.json.{JsonCodec}
import std.sync.{Locked}
import std.task.{withTimeout}

extern fn lyraHttpListen(host: String, port: Int, options: CPtr): CPtr;
extern fn lyraHttpOptions(capacity: Int, requestTimeoutMs: Int, idleReadTimeoutMs: Int, writeTimeoutMs: Int, handshakeTimeoutMs: Int, graceMs: Int, claimTimeoutMs: Int, maxBodyBytes: Int, maxRequestBytes: Int, keepAliveTimeoutMs: Int, maxRequestsPerConnection: Int, streamRequestBody: Bool): CPtr;
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

pub enum HttpErrorKind {
    System(Int), Invalid, Closed, Busy, Timeout, Malformed, TooLarge, InvalidUtf8, InvalidJson, DuplicateHeader,
    TlsConfig, HandshakeFailed, HandshakeTimeout, UnsupportedExpectation,
}

pub struct HttpError(pub val kind: HttpErrorKind, pub val operation: String, pub val detail: String = "") {
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

pub struct HttpServerOptions(
    pub val capacity: Int = 4,
    pub val requestTimeoutMs: Int = 30000,
    pub val idleReadTimeoutMs: Int? = None,
    pub val writeTimeoutMs: Int = 5000,
    pub val handshakeTimeoutMs: Int = 5000,
    pub val graceMs: Int = 5000,
    pub val claimTimeoutMs: Int = 5000,
    pub val maxBodyBytes: Int = 8388608,
    pub val maxRequestBytes: Int = 16777216,
    pub val keepAliveTimeoutMs: Int = 5000,
    pub val maxRequestsPerConnection: Int = 1000,
    pub val streamRequestBody: Bool = false,
)

fn serverOptions(options: HttpServerOptions): CPtr {
    val idle = match options.idleReadTimeoutMs {
        None => -1,
        Some(milliseconds) => if milliseconds < 1 { 0 } else { milliseconds },
    }
    lyraHttpOptions(options.capacity, options.requestTimeoutMs, idle, options.writeTimeoutMs, options.handshakeTimeoutMs, options.graceMs, options.claimTimeoutMs, options.maxBodyBytes, options.maxRequestBytes, options.keepAliveTimeoutMs, options.maxRequestsPerConnection, options.streamRequestBody)
}

fn acceptResult(request: CPtr): Result<Request, HttpError> {
    val status = lyraHttpRequestStatus(request)
    if status == 0 { Ok(Request(request)) } else { Err(httpError("accept", status)) }
}

pub class Listener {
    val handle: CPtr
    init(handle: CPtr) { this.handle = handle }

    pub fn port(): Int { lyraHttpPort(handle) }
    pub async fn accept(timeoutMs: Int = -1): Result<Request, HttpError> {
        acceptResult(await lyraHttpAcceptAsync(handle, timeoutMs, false, true))
    }
    pub async fn withAccepted<T>(timeoutMs: Int, handleRequest: (Request) -> Task<T>): Result<T, HttpError> {
        val request = acceptResult(await lyraHttpAcceptAsync(handle, timeoutMs, true, true))?
        Ok(await handleRequest(request))
    }
    pub fn close(): Unit { lyraHttpStop(handle) }
    pub async fn closeAndWait(): Unit { await lyraHttpListenerCloseAsync(handle) }
}

pub struct Header(pub val name: String, pub val value: String)

pub class ResponseWriter {
    val handle: CPtr
    init(handle: CPtr) { this.handle = handle }

    pub async fn write(content: Bytes): Result<Unit, HttpError> {
        val status = await lyraHttpWriteStreamAsync(handle, content, false)
        if status == 0 { Ok({}) } else { Err(httpError("response.write", status)) }
    }
}

pub enum ResponseBody {
    Buffered(Bytes), Streaming((ResponseWriter) -> Task<Result<Unit, HttpError>>),
}

pub struct Response(pub val code: Int, pub val contentType: String, pub val body: ResponseBody, pub val headers: FrozenList<Header> = FrozenList<Header>([])) {
    pub static fn text(code: Int, body: String): Response { Response.bytes(code, "text/plain", body.toBytes()) }
    pub static fn bytes(code: Int, contentType: String, body: Bytes): Response { Response(code, contentType, ResponseBody.Buffered(body)) }
    pub static fn stream(code: Int, contentType: String, produce: (ResponseWriter) -> Task<Result<Unit, HttpError>>): Response {
        Response(code, contentType, ResponseBody.Streaming(produce))
    }
    pub static fn json<T>(value: T, codec: JsonCodec<T>, code: Int = 200): Response {
        Response.bytes(code, "application/json", codec.encode(value).toBytes())
    }
    pub fn withHeader(name: String, value: String): Response {
        val fields: List<Header> = []
        for field in headers { fields.add(field) }
        fields.add(Header(name, value))
        Response(code, contentType, body, FrozenList(fields))
    }
}

pub class Request {
    val handle: CPtr
    init(handle: CPtr) { this.handle = handle }

    pub fn method(): String { lyraHttpMethod(handle) }
    pub fn target(): String { lyraHttpTarget(handle) }
    pub fn bodyStreaming(): Bool { lyraHttpBodyStreaming(handle) }
    pub fn body(): Bytes {
        if bodyStreaming() { throw Exception("request.body is buffered-only; use await request.readBody()") }
        lyraHttpBody(handle)
    }
    pub async fn readBody(): Result<Bytes?, HttpError> {
        val result = await lyraHttpReadBodyAsync(handle)
        val status = lyraHttpBodyReadStatus(result)
        if status != 0 { return Err(httpError("request.readBody", status)) }
        Ok(if lyraHttpBodyReadHasBytes(result) { Some(lyraHttpBodyReadBytes(result)) } else { None })
    }
    pub fn text(): Result<String, HttpError> {
        if bodyStreaming() { return Err(HttpError(HttpErrorKind.Invalid, "request.text", "use await request.readBody()")) }
        match body().toString() {
            Some(text) => Ok(text),
            None => Err(HttpError(HttpErrorKind.InvalidUtf8, "request.text")),
        }
    }
    pub fn path(name: String): String? {
        val index = lyraHttpPathIndex(handle, name)
        if index < 0 { None } else { Some(lyraHttpPathValue(handle, index)) }
    }
    pub fn pathInt(name: String): Result<Int?, HttpError> {
        val index = lyraHttpPathIndex(handle, name)
        if index < 0 { return Ok(None) }
        val parsed = lyraHttpParseInt(lyraHttpPathValue(handle, index))
        if lyraHttpIntegerValid(parsed) { Ok(Some(lyraHttpIntegerValue(parsed))) }
        else { Err(HttpError(HttpErrorKind.Invalid, "request.pathInt", "invalid integer path parameter: " + name)) }
    }
    pub fn queryAll(name: String): List<String> {
        val values: List<String> = []
        for index in 0..<lyraHttpQueryCount(handle) {
            if lyraHttpQueryName(handle, index) == name { values.add(lyraHttpQueryValue(handle, index)) }
        }
        values
    }
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
    pub fn headers(name: String): List<String> {
        val values: List<String> = []
        for index in 0..<lyraHttpHeaderCount(handle) {
            if lyraHttpHeaderMatches(handle, index, name) { values.add(lyraHttpHeaderValue(handle, index)) }
        }
        values
    }
    pub fn cookie(name: String): String? {
        val value = lyraHttpCookie(handle, name)
        if value.isNull() { None } else { Some(lyraHttpCookieValue(value)) }
    }
    pub fn json<T>(codec: JsonCodec<T>): Result<T, HttpError> {
        val input = text()?
        match codec.decode(input) {
            Ok(value) => Ok(value),
            Err(detail) => Err(HttpError(HttpErrorKind.InvalidJson, "request.json", detail)),
        }
    }
    pub fn disconnected(): Bool { lyraHttpDisconnected(handle) }
    pub async fn waitDisconnect(timeoutMs: Int = -1): Bool {
        val status = await lyraHttpWaitDisconnectAsync(handle, timeoutMs)
        if status < 0 { throw Exception(httpError("request.disconnect", -status).message()) }
        status == 1
    }
    pub fn claim(): Result<Unit, HttpError> {
        val status = lyraHttpClaim(handle)
        if status == 0 { Ok({}) } else { Err(httpError("request.claim", status)) }
    }
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
                if method() != "HEAD" { (await produce(ResponseWriter(handle)))? }
                await lyraHttpWriteStreamAsync(handle, "".toBytes(), true)
            },
        }
        if status == 0 { Ok({}) } else { Err(httpError("respond", status)) }
    }
    pub fn close(): Unit { lyraHttpDrop(handle) }
    pub async fn closeAndWait(): Unit { await lyraHttpRequestCloseAsync(handle) }
}

fn listenerResult(handle: CPtr, operation: String): Result<Listener, HttpError> {
    val status = lyraHttpStatus(handle)
    if status == 0 { Ok(Listener(handle)) } else { Err(httpError(operation, status, lyraHttpStatusDetail(handle))) }
}

pub fn listen(port: Int, host: String = "127.0.0.1", options: HttpServerOptions = HttpServerOptions()): Result<Listener, HttpError> {
    listenerResult(lyraHttpListen(host, port, serverOptions(options)), "listen")
}

pub struct RouteRegistration(pub val method: String, pub val path: String, pub val handler: (Request) -> Task<Response>, pub val origin: String = "manual registration")

struct Route(val registration: RouteRegistration, val pattern: CPtr)

fn describeRoute(route: RouteRegistration): String { "${route.method} ${route.path} (${route.origin})" }

pub class Shutdown {
    val signal: Channel<Unit> = Channel<Unit>(1)
    pub fn request(): Unit { signal.close() }
    async fn wait(): Unit { val _ = await signal.receive() }
}

pub class Router {
    val routes: Locked<List<Route>> = Locked([])

    pub fn get(pattern: String, handler: (Request) -> Task<Response>): Unit { route("GET", pattern, handler) }
    pub fn post(pattern: String, handler: (Request) -> Task<Response>): Unit { route("POST", pattern, handler) }
    pub fn put(pattern: String, handler: (Request) -> Task<Response>): Unit { route("PUT", pattern, handler) }
    pub fn delete(pattern: String, handler: (Request) -> Task<Response>): Unit { route("DELETE", pattern, handler) }
    pub fn patch(pattern: String, handler: (Request) -> Task<Response>): Unit { route("PATCH", pattern, handler) }
    pub fn options(pattern: String, handler: (Request) -> Task<Response>): Unit { route("OPTIONS", pattern, handler) }
    pub fn head(pattern: String, handler: (Request) -> Task<Response>): Unit { route("HEAD", pattern, handler) }

    pub fn route(method: String, pattern: String, handler: (Request) -> Task<Response>): Unit {
        mount([RouteRegistration(method, pattern, handler)])
    }

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

    pub async fn serve(listener: Listener, shutdown: Shutdown? = None): Unit {
        lyraHttpKeepListener(listener.handle)
        val snapshot = routes.snapshot()
        taskGroup {
            spawn lyraHttpListenerScopeAsync(listener.handle);
            val slots: List<Task<Unit>> = []
            for slot in 0..<lyraHttpCapacity(listener.handle) { slots.add(spawn serveSlot(listener, snapshot)) }
            if let Some(stop) = shutdown {
                val stopping = spawn drainOnShutdown(listener, stop, slots)
                await joinSlots(slots)
                stopping.cancel()
            } else { await joinSlots(slots) }
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
        for slot in slots { slot.cancel() }
    }
}

async fn joinSlots(slots: List<Task<Unit>>): Unit {
    for slot in slots { val _ = awaitOutcome(slot) }
}
