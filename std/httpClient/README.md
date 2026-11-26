# `std.httpClient`

```lyra
import std.httpClient.{HttpClientOptions}
import std.httpClient.{httpGet}

async fn main(): Unit {
    val result = await httpGet("https://example.com", HttpClientOptions(timeoutMs: 4000))
    match result {
        Ok(response) => print("HTTP ${response.status}: ${response.text() ?? "<binary>"}"),
        Err(problem) => print(problem.message()),
    }
}
```

`httpRequest(method, url, headers: [...], body: Some(bytes), options: ...)` 发送通用 HTTP/HTTPS 请求；`httpGet` 是无请求体的简写。`ClientResponse` 保留状态码、原始 `Bytes` 响应体和有序的重复响应头。`header(name)` 取第一个，`headerValues(name)` 取全部，匹配时不区分 ASCII 大小写。`text()` 在响应体不是合法 UTF-8 时返回 `None`。HTTP `4xx` 和 `5xx` 也是正常响应，连接、证书、超时等故障才返回 `HttpClientError`。

- 要求绝对 URL，URL 不接受用户信息。请求的 `Host`、`Content-Length`、`Transfer-Encoding`、逐跳连接头和代理认证由传输层管理。`GET` 和 `HEAD` 不接受请求体。
- 默认连接超时 5 秒、整次请求超时 30 秒，响应头和响应体合计最多 8 MiB。两个超时为 1 至 2147483647 毫秒，`maxResponseBytes` 为正整数字节。总超时包含等待连接的时间，多跳请求共用一个总超时与字节预算。
- 默认不跟随重定向。将 `maxRedirects` 设置为 `1`–`20` 可跟随同源 `GET`/`HEAD` 跳转；跨协议、主机或端口的目标会被拒绝。
- `proxy` 可显式指定 HTTP/HTTPS 代理；不自动读取代理环境变量。HTTPS 始终验证主机名与证书；默认用系统信任根，也可用 `caPem: Some(bytes)` 指定专用 PEM 信任根。错误信息不打印目标或代理地址。
- 调用返回冷 Task，激活后由独立 libuv 事件循环驱动 libcurl Multi socket API。连接池跨请求复用，最多同时处理 128 个请求、建立 64 条连接，每个目标最多 16 条连接。超过请求容量返回 `HttpClientErrorKind.Busy`，应用可以据此实施背压。
- HTTPS 通过 ALPN 优先使用 HTTP/2，并按服务端协商结果使用 HTTP/1.1。同一 HTTP/2 连接允许最多 64 个并发流，同时受服务端公布的流容量限制。静态 libcurl、nghttp2、zlib 和 OpenSSL 随 SDK 分发，连接池支持多路复用。
- 取消通过事件循环移除传输，等本次原生操作退出后完成 Task。HTTP 请求不占用文件 I/O 的阻塞池。`httpRequest` 和 `httpGet` 完整读取正文后返回快照。

## 流式响应

`withHttpResponse` 在收到最终响应头后调用异步消费函数，函数退出后等待原生连接交接或关闭。消费函数的显式返回类型同时确定泛型结果与错误传播目标。

```lyra
import std.httpClient.{ClientStream, HttpClientError, HttpClientOptions, withHttpResponse}

async fn downloadSize(url: String): Result<Int, HttpClientError> {
    await withHttpResponse("GET", url, async (response: ClientStream): Result<Int, HttpClientError> => {
        var size = 0
        while true {
            match (await response.read())? {
                Some(bytes) => { size += bytes.size },
                None => break,
            }
        }
        Ok(size)
    }, options: HttpClientOptions(maxResponseBytes: 64 * 1024 * 1024))
}
```

- `ClientStream` 提供与完整响应相同的 `status`、`headers`、`header` 和 `headerValues`。`read()` 返回一个不可变字节块，正常结束为 `Ok(None)`；网络分块边界由传输决定。
- 运行时至多保留一个待消费的正文块，尚未消费时暂停 libcurl 接收并传递网络背压。`maxResponseBytes` 仍限制响应头和完整正文的累计字节数；总超时包含消费函数暂停读取的时间。
- 每条响应最多一个挂起的读取，竞争者得到 `Busy`。显式 `close()` 等待当前传输关闭，挂起与后续读取得到 `Closed`。取消读取也关闭所属传输。
- 读完正文后连接可以复用。提前返回、业务错误、异常和取消都会关闭未完成的传输，作用域结束后保留的 `ClientStream` 读取返回 `Closed`。
