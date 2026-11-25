# std.httpTls：HTTPS 服务端

`listenTls(port, certificateChain, privateKey, host: "127.0.0.1", handshakeTimeoutMs: 5000)` 返回 `Result<Listener,HttpError>`。证书链和未加密私钥从 `Bytes` 读取，先检查 PEM、密钥是否匹配和握手预算，再绑定端口。证书链按叶证书到中间证书的顺序提供；不要把私钥提交到仓库。

仅接受 TLS 1.2/1.3；握手与 HTTP 请求读取各有独立截止时间。连接完成握手后，用 `std.http.Router` 的同一个注册和服务 API。TLS 握手、请求、响应和断线都遵守有界并发；失败连接被关闭，不中断监听。只使用明文 API 的程序不包含 OpenSSL。
