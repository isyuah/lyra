extern fn lyraHttpTlsListen(host: String, port: Int, certificateChain: Bytes, privateKey: Bytes, options: CPtr): CPtr;

/// 校验证书链和未加密私钥的 PEM 后绑定 HTTPS 端口。证书链从叶证书起，依次提供中间证书。
/// 支持 TLS 1.2/1.3，默认优先协商 HTTP/2，返回可交给普通 `Router` 的监听器。
pub fn listenTls(port: Int, certificateChain: Bytes, privateKey: Bytes, host: String = "127.0.0.1", options: HttpServerOptions = HttpServerOptions()): Result<Listener,HttpError> {
    listenerResult(lyraHttpTlsListen(host, port, certificateChain, privateKey, serverOptions(options)), "listenTls")
}
