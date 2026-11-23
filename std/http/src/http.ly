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
    pub fn status(self): Int { lyraHttpStatus(self.handle) }
    pub fn port(self): Int { lyraHttpPort(self.handle) }
    pub fn accept(self, timeoutMs: Int): Request { Request(lyraHttpAccept(self.handle, timeoutMs)) }
    pub fn close(self): Unit { lyraHttpStop(self.handle) }
}

pub struct Response(pub val code: Int, pub val contentType: String, pub val body: String)

pub class Request(val handle: CPtr) {
    pub fn status(self): Int { lyraHttpRequestStatus(self.handle) }
    pub fn method(self): String { lyraHttpMethod(self.handle) }
    pub fn target(self): String { lyraHttpTarget(self.handle) }
    pub fn body(self): String { lyraHttpBody(self.handle) }
    pub fn pathInt(self, prefix: String, fallback: Int): Int { lyraHttpPathInt(self.target(), prefix, fallback) }
    pub fn disconnected(self): Bool { lyraHttpDisconnected(self.handle) }
    pub fn waitDisconnect(self, timeoutMs: Int): Bool { lyraHttpWaitDisconnect(self.handle, timeoutMs) }
    pub fn watchDisconnect(self): Bool { lyraHttpWatchDisconnect(self.handle) }
    pub fn respond(self, code: Int, mediaType: String, body: String): Int { lyraHttpRespond(self.handle, code, mediaType, body) }
    pub fn close(self): Unit { lyraHttpDrop(self.handle) }
}

pub fn listen(port: Int): Listener { Listener(lyraHttpListen(port)) }
