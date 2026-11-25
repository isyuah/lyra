extern fn lyraHttpTlsListen(host: String, port: Int, certificateChain: Bytes, privateKey: Bytes, handshakeTimeoutMs: Int): CPtr;

pub fn listenTls(port: Int, certificateChain: Bytes, privateKey: Bytes, host: String = "127.0.0.1", handshakeTimeoutMs: Int = 5000): Result<Listener,HttpError> {
    listenerResult(lyraHttpTlsListen(host, port, certificateChain, privateKey, handshakeTimeoutMs), "listenTls")
}
