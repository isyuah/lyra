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

pub class Listener(val handle: CPtr) {
    pub fn status(): Int { lyraHttpStatus(handle) }
    pub fn port(): Int { lyraHttpPort(handle) }
    pub fn accept(timeoutMs: Int): Request { Request(lyraHttpAccept(handle, timeoutMs)) }
    pub fn close(): Unit { lyraHttpStop(handle) }
}

pub struct Response(pub val code: Int, pub val contentType: String, pub val body: String)

pub class Request(val handle: CPtr) {
    pub fn status(): Int { lyraHttpRequestStatus(handle) }
    pub fn method(): String { lyraHttpMethod(handle) }
    pub fn target(): String { lyraHttpTarget(handle) }
    pub fn body(): String { lyraHttpBody(handle) }
    pub fn pathInt(prefix: String, fallback: Int): Int { lyraHttpPathInt(target(), prefix, fallback) }
    pub fn disconnected(): Bool { lyraHttpDisconnected(handle) }
    pub fn waitDisconnect(timeoutMs: Int): Bool { lyraHttpWaitDisconnect(handle, timeoutMs) }
    pub fn watchDisconnect(): Bool { lyraHttpWatchDisconnect(handle) }
    pub fn respond(code: Int, mediaType: String, body: String): Int { lyraHttpRespond(handle, code, mediaType, body) }
    pub fn close(): Unit { lyraHttpDrop(handle) }
}

pub fn listen(port: Int): Listener { Listener(lyraHttpListen(port)) }
