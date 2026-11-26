import std.io.{writeError}
import std.json.{JsonCodec}
import std.task.{withTimeout}
import std.time.{Clock}

extern fn lyraHttpListen(host: String, port: Int): CPtr;
extern fn lyraHttpStatus(server: CPtr): Int;
extern fn lyraHttpStatusDetail(server: CPtr): String;
extern fn lyraHttpErrorKind(status: Int): Int;
extern fn lyraHttpPort(server: CPtr): Int;
extern fn lyraHttpBeginDrain(server: CPtr): Unit;
extern fn lyraHttpStop(server: CPtr): Int;
extern fn lyraHttpAccept(server: CPtr, timeoutMs: Int): CPtr;
extern fn lyraHttpRequestStatus(request: CPtr): Int;
extern fn lyraHttpMethod(request: CPtr): String;
extern fn lyraHttpTarget(request: CPtr): String;
extern fn lyraHttpBody(request: CPtr): Bytes;
extern fn lyraHttpDisconnected(request: CPtr): Int;
extern fn lyraHttpWaitDisconnect(request: CPtr, timeoutMs: Int): Int;
extern fn lyraHttpWatchDisconnect(request: CPtr): Bool;
extern fn lyraHttpDrop(request: CPtr): Int;
extern fn lyraHttpResponseHeaders(count: Int): CPtr;
extern fn lyraHttpResponseHeaderSet(headers: CPtr, index: Int, name: String, value: String): Int;
extern fn lyraHttpRespond(request: CPtr, code: Int, mediaType: String, body: Bytes, headers: CPtr): Int;
extern fn lyraHttpAcceptCursor(server: CPtr, timeoutMs: Int): CPtr;
extern fn lyraHttpAcceptAdvance(cursor: CPtr): Int;
extern fn lyraHttpAcceptTake(cursor: CPtr): CPtr;
extern fn lyraHttpAcceptDrop(cursor: CPtr): Unit;
extern fn lyraHttpRouteMethodValid(method: String): Bool;
extern fn lyraHttpRoutePattern(route: String): CPtr;
extern fn lyraHttpRouteValid(route: CPtr): Bool;
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

pub exception Busy

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

fn closeHandle(status: Int, operation: String): Unit {
    if status != 0 { throw Busy(httpError(operation, status).message()) }
}

fn observedDisconnect(status: Int): Bool {
    if status < 0 { throw Busy(httpError("request.disconnect", -status).message()) }
    status == 1
}

pub class Listener {
    val handle: CPtr
    init(handle: CPtr) { this.handle = handle }

    pub fn port(): Int { lyraHttpPort(handle) }
    pub fn accept(timeoutMs: Int): Result<Request, HttpError> {
        val request = lyraHttpAccept(handle, timeoutMs)
        val status = lyraHttpRequestStatus(request)
        if status == 0 { Ok(Request(request)) } else { Err(httpError("accept", status)) }
    }
    pub async fn withAccepted<T>(timeoutMs: Int, handleRequest: (Request) -> Task<T>): Result<T, HttpError> {
        if timeoutMs < 1 || timeoutMs > 2147483647 { return Err(HttpError(HttpErrorKind.Invalid, "accept")) }
        val cursor = lyraHttpAcceptCursor(handle, timeoutMs)
        defer lyraHttpAcceptDrop(cursor)
        await acceptScoped<T>(cursor, handleRequest)
    }
    pub fn close(): Unit { closeHandle(lyraHttpStop(handle), "listener.close") }
}

pub struct Header(pub val name: String, pub val value: String)

pub struct Response(pub val code: Int, pub val contentType: String, pub val body: Bytes, pub val headers: FrozenList<Header> = FrozenList<Header>([])) {
    pub static fn text(code: Int, body: String): Response { Response(code, "text/plain", body.toBytes()) }
    pub static fn bytes(code: Int, contentType: String, body: Bytes): Response { Response(code, contentType, body) }
    pub static fn json<T>(value: T, codec: JsonCodec<T>, code: Int = 200): Response {
        Response(code, "application/json", codec.encode(value).toBytes())
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
    pub fn body(): Bytes { lyraHttpBody(handle) }
    pub fn text(): Result<String, HttpError> {
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
    pub fn disconnected(): Bool { observedDisconnect(lyraHttpDisconnected(handle)) }
    pub fn waitDisconnect(timeoutMs: Int): Bool { observedDisconnect(lyraHttpWaitDisconnect(handle, timeoutMs)) }
    pub fn watchDisconnect(): Bool { lyraHttpWatchDisconnect(handle) }
    pub fn respond(response: Response): Result<Unit, HttpError> {
        val headers = lyraHttpResponseHeaders(response.headers.size)
        if headers.isNull() { return Err(HttpError(HttpErrorKind.TooLarge, "respond")) }
        for index in 0..<response.headers.size {
            val field = response.headers[index]
            val status = lyraHttpResponseHeaderSet(headers, index, field.name, field.value)
            if status != 0 { return Err(httpError("respond", status)) }
        }
        val status = lyraHttpRespond(handle, response.code, response.contentType, response.body, headers)
        if status == 0 { Ok({}) } else { Err(httpError("respond", status)) }
    }
    pub fn close(): Unit { closeHandle(lyraHttpDrop(handle), "request.close") }
}

fn listenerResult(handle: CPtr, operation: String): Result<Listener, HttpError> {
    val status = lyraHttpStatus(handle)
    if status == 0 { Ok(Listener(handle)) } else { Err(httpError(operation, status, lyraHttpStatusDetail(handle))) }
}

pub fn listen(port: Int, host: String = "127.0.0.1"): Result<Listener, HttpError> {
    listenerResult(lyraHttpListen(host, port), "listen")
}

struct Route(val method: String, val pattern: CPtr, val handler: (Request) -> Task<Response>)

pub class Shutdown {
    val signal: Channel<Unit> = Channel<Unit>(1)
    pub fn request(): Unit { signal.close() }
    async fn wait(): Unit { val _ = await signal.receive() }
}

pub class Router {
    val routes: List<Route> = []

    pub fn get(pattern: String, handler: (Request) -> Task<Response>): Unit { route("GET", pattern, handler) }
    pub fn post(pattern: String, handler: (Request) -> Task<Response>): Unit { route("POST", pattern, handler) }
    pub fn put(pattern: String, handler: (Request) -> Task<Response>): Unit { route("PUT", pattern, handler) }
    pub fn delete(pattern: String, handler: (Request) -> Task<Response>): Unit { route("DELETE", pattern, handler) }
    pub fn patch(pattern: String, handler: (Request) -> Task<Response>): Unit { route("PATCH", pattern, handler) }
    pub fn options(pattern: String, handler: (Request) -> Task<Response>): Unit { route("OPTIONS", pattern, handler) }
    pub fn head(pattern: String, handler: (Request) -> Task<Response>): Unit { route("HEAD", pattern, handler) }

    pub fn route(method: String, pattern: String, handler: (Request) -> Task<Response>): Unit {
        if !lyraHttpRouteMethodValid(method) { throw Exception("unsupported HTTP method: " + method) }
        val compiled = lyraHttpRoutePattern(pattern)
        if !lyraHttpRouteValid(compiled) { throw Exception("invalid HTTP route: " + pattern) }
        routes.add(Route(method, compiled, handler))
    }

    pub async fn serve(listener: Listener, capacity: Int = 4, shutdown: Shutdown? = None, graceMs: Int = 5000): Unit {
        if capacity < 1 || capacity > 64 { throw Exception("HTTP capacity must be in 1..64") }
        if graceMs < 0 { throw Exception("HTTP graceMs must be nonnegative") }
        defer listener.close()
        val snapshot: List<Route> = []
        for route in routes { snapshot.add(route) }
        taskGroup {
            val slots: List<Task<Unit>> = []
            for slot in 0..<capacity { slots.add(spawn serveSlot(listener, snapshot)) }
            if let Some(stop) = shutdown {
                await stop.wait()
                val started = Clock.monotonicMs()
                lyraHttpBeginDrain(listener.handle)
                var status = lyraHttpStop(listener.handle)
                while lyraHttpErrorKind(status) == 10 {
                    await sleepMs(1)
                    status = lyraHttpStop(listener.handle)
                }
                closeHandle(status, "listener.close")
                val elapsed = Clock.monotonicMs() - started
                val remaining = if elapsed >= graceMs { 0 } else { graceMs - elapsed }
                if let Cancelled(_) = awaitOutcome(withTimeout(remaining, joinSlots(slots))) {
                    for slot in slots { slot.cancel() }
                }
            }
        }
    }
}

fn allow(methods: List<String>, method: String): Unit {
    for existing in methods { if existing == method { return } }
    methods.add(method)
}

async fn dispatch(request: Request, routes: List<Route>): Response {
    val method = request.method()
    val allowed: List<String> = []
    var headFallback: Route? = None
    for route in routes {
        if lyraHttpRouteMatch(request.handle, route.pattern) {
            if method == route.method {
                lyraHttpRouteBind(request.handle, route.pattern)
                return await route.handler(request)
            }
            allow(allowed, route.method)
            if route.method == "GET" {
                allow(allowed, "HEAD")
                if method == "HEAD" && headFallback == None { headFallback = Some(route) }
            }
        }
    }
    if let Some(route) = headFallback {
        lyraHttpRouteBind(request.handle, route.pattern)
        return await route.handler(request)
    }
    if allowed.size == 0 { return Response.text(404, "not found") }
    var methods = allowed[0]
    for index in 1..<allowed.size { methods += ", " + allowed[index] }
    Response.text(405, "method not allowed").withHeader("Allow", methods)
}

async fn respond(request: Request, routes: List<Route>): Unit {
    val response = if request.watchDisconnect() {
        try { await dispatch(request, routes) } catch {
            val _ = writeError("HTTP handler raised an exception\n")
            Response.text(500, "internal server error")
        }
    } else { Response.text(500, "disconnect watch unavailable") }
    if request.disconnected() { return }
    val sent = await blocking { request.respond(response) }
    if let Err(error) = sent {
        val _ = writeError("HTTP response failed: ${error.message()}\n")
        if error.kind == HttpErrorKind.Invalid || error.kind == HttpErrorKind.TooLarge {
            val _ = await blocking { request.respond(Response.text(500, "internal server error")) }
        }
    }
}

async fn acceptScoped<T>(cursor: CPtr, handleRequest: (Request) -> Task<T>): Result<T, HttpError> {
    var status = 0
    while status == 0 {
        status = await parallel { lyraHttpAcceptAdvance(cursor) }
    }
    if status < 0 { return Err(httpError("accept", -status)) }
    val handle = lyraHttpAcceptTake(cursor)
    if handle.isNull() { return Err(HttpError(HttpErrorKind.Closed, "accept")) }
    val request = Request(handle)
    defer request.close()
    Ok(await handleRequest(request))
}

async fn serveSlot(listener: Listener, routes: List<Route>): Unit {
    val cursor = lyraHttpAcceptCursor(listener.handle, -1)
    defer lyraHttpAcceptDrop(cursor)
    while true {
        val accepted = await acceptScoped<Unit>(cursor, async (request: Request) => {
            taskGroup { spawn respond(request, routes); };
        })
        if let Err(error) = accepted {
            if error.kind == HttpErrorKind.Closed { return }
            throw Exception(error.message())
        }
    }
}

async fn joinSlots(slots: List<Task<Unit>>): Unit {
    for slot in slots { await slot }
}
