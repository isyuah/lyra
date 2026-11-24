import std.io.writeError
import std.json.JsonCodec

extern fn lyraHttpListen(port: Int): CPtr;
extern fn lyraHttpStatus(server: CPtr): Int;
extern fn lyraHttpPort(server: CPtr): Int;
extern fn lyraHttpStop(server: CPtr): Unit;
extern fn lyraHttpAccept(server: CPtr, timeoutMs: Int): CPtr;
extern fn lyraHttpRequestStatus(request: CPtr): Int;
extern fn lyraHttpMethod(request: CPtr): String;
extern fn lyraHttpTarget(request: CPtr): String;
extern fn lyraHttpBody(request: CPtr): String;
extern fn lyraHttpDisconnected(request: CPtr): Bool;
extern fn lyraHttpWaitDisconnect(request: CPtr, timeoutMs: Int): Bool;
extern fn lyraHttpWatchDisconnect(request: CPtr): Bool;
extern fn lyraHttpDrop(request: CPtr): Unit;
extern fn lyraHttpRespond(request: CPtr, code: Int, mediaType: String, body: String): Int;
extern fn lyraHttpPathInt(target: String, prefix: String, fallback: Int): Int;
extern fn lyraHttpAcceptCursor(server: CPtr): CPtr;
extern fn lyraHttpAcceptAdvance(cursor: CPtr): Int;
extern fn lyraHttpAcceptTake(cursor: CPtr): CPtr;
extern fn lyraHttpAcceptDrop(cursor: CPtr): Unit;
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

pub class Listener(val handle: CPtr) {
    pub fn status(): Int { lyraHttpStatus(handle) }
    pub fn port(): Int { lyraHttpPort(handle) }
    pub fn accept(timeoutMs: Int): Request { Request(lyraHttpAccept(handle, timeoutMs)) }
    pub fn close(): Unit { lyraHttpStop(handle) }
}

pub struct Response(pub val code: Int, pub val contentType: String, pub val body: String) {
    pub static fn text(code: Int, body: String): Response { Response(code, "text/plain", body) }
    pub static fn json<T>(value: T, codec: JsonCodec<T>, code: Int = 200): Response {
        Response(code, "application/json", codec.encode(value))
    }
}

pub class Request(val handle: CPtr) {
    pub fn status(): Int { lyraHttpRequestStatus(handle) }
    pub fn method(): String { lyraHttpMethod(handle) }
    pub fn target(): String { lyraHttpTarget(handle) }
    pub fn body(): String { lyraHttpBody(handle) }
    pub fn pathInt(prefix: String, fallback: Int): Int { lyraHttpPathInt(target(), prefix, fallback) }
    pub fn path(name: String): String? {
        val index = lyraHttpPathIndex(handle, name)
        if index < 0 { None } else { Some(lyraHttpPathValue(handle, index)) }
    }
    pub fn pathInt(name: String): Result<Int?, String> {
        val index = lyraHttpPathIndex(handle, name)
        if index < 0 { return Ok(None) }
        val parsed = lyraHttpParseInt(lyraHttpPathValue(handle, index))
        if lyraHttpIntegerValid(parsed) { Ok(Some(lyraHttpIntegerValue(parsed))) } else { Err("invalid integer path parameter: " + name) }
    }
    pub fn queryAll(name: String): List<String> {
        val values: List<String> = []
        for index in 0..<lyraHttpQueryCount(handle) {
            if lyraHttpQueryName(handle, index) == name { values.add(lyraHttpQueryValue(handle, index)) }
        }
        values
    }
    pub fn json<T>(codec: JsonCodec<T>): Result<T, String> { codec.decode(body()) }
    pub fn disconnected(): Bool { lyraHttpDisconnected(handle) }
    pub fn waitDisconnect(timeoutMs: Int): Bool { lyraHttpWaitDisconnect(handle, timeoutMs) }
    pub fn watchDisconnect(): Bool { lyraHttpWatchDisconnect(handle) }
    pub fn respond(code: Int, mediaType: String, body: String): Int { lyraHttpRespond(handle, code, mediaType, body) }
    pub fn close(): Unit { lyraHttpDrop(handle) }
}

pub fn listen(port: Int): Listener { Listener(lyraHttpListen(port)) }

struct Route(val method: String, val pattern: CPtr, val handler: (Request) -> Task<Response>)

pub class Router {
    val routes: List<Route> = []

    pub fn get(pattern: String, handler: (Request) -> Task<Response>): Unit { register("GET", pattern, handler) }
    pub fn post(pattern: String, handler: (Request) -> Task<Response>): Unit { register("POST", pattern, handler) }

    fn register(method: String, pattern: String, handler: (Request) -> Task<Response>): Unit {
        val compiled = lyraHttpRoutePattern(pattern)
        if !lyraHttpRouteValid(compiled) { throw Exception("invalid HTTP route: " + pattern) }
        routes.add(Route(method, compiled, handler))
    }

    pub async fn serve(listener: Listener, capacity: Int = 4): Unit {
        if capacity < 1 || capacity > 64 { throw Exception("HTTP capacity must be in 1..64") }
        if listener.status() != 0 { throw Exception("HTTP listener is unavailable") }
        defer listener.close()
        val snapshot: List<Route> = []
        for route in routes { snapshot.add(route) }
        taskGroup {
            for slot in 0..<capacity { spawn serveSlot(listener, snapshot); }
        }
    }
}

async fn dispatch(request: Request, routes: List<Route>): Response {
    var pathMatched = false
    for route in routes {
        if lyraHttpRouteMatch(request.handle, route.pattern) {
            pathMatched = true
            if request.method() == route.method {
                lyraHttpRouteBind(request.handle, route.pattern)
                return await route.handler(request)
            }
        }
    }
    if pathMatched { Response.text(405, "method not allowed") } else { Response.text(404, "not found") }
}

async fn respond(request: Request, routes: List<Route>): Unit {
    val response = if request.watchDisconnect() {
        try { await dispatch(request, routes) } catch error: Exception {
            writeError("HTTP ${request.target()}: ${error.message}\n")
            Response.text(500, "internal server error")
        }
    } else { Response.text(500, "disconnect watch unavailable") }
    if !request.disconnected() {
        await blocking { request.respond(response.code, response.contentType, response.body); };
    }
}

async fn serveSlot(listener: Listener, routes: List<Route>): Unit {
    val cursor = lyraHttpAcceptCursor(listener.handle)
    defer lyraHttpAcceptDrop(cursor)
    while true {
        val status = await parallel { lyraHttpAcceptAdvance(cursor) }
        if status < 0 { throw Exception("HTTP accept failed: ${-status}") }
        if status == 1 {
            val request = Request(lyraHttpAcceptTake(cursor))
            defer request.close()
            taskGroup { spawn respond(request, routes); };
        }
    }
}
