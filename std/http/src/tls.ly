extern fn lyraHttpTlsListen(host: String, port: Int, certificateChain: Bytes, privateKey: Bytes, options: CPtr): CPtr;

pub fn listenTls(port: Int, certificateChain: Bytes, privateKey: Bytes, host: String = "127.0.0.1", options: HttpServerOptions = HttpServerOptions()): Result<Listener,HttpError> {
    listenerResult(lyraHttpTlsListen(host, port, certificateChain, privateKey, serverOptions(options)), "listenTls")
}
