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
- 默认连接超时 5 秒、整次请求超时 30 秒；响应头和响应体合计最多 8 MiB。两个超时为 1 至 2147483647 毫秒，`maxResponseBytes` 为正整数字节。总超时包含等待阻塞池的时间；若请求经过多跳，仍共用一个总超时与字节预算。
- 默认不跟随重定向。将 `maxRedirects` 设置为 `1`–`20` 可跟随同源 `GET`/`HEAD` 跳转；跨协议、主机或端口的目标会被拒绝。
- `proxy` 可显式指定 HTTP/HTTPS 代理；不自动读取代理环境变量。HTTPS 始终验证主机名与证书；默认用系统信任根，也可用 `caPem: Some(bytes)` 指定专用 PEM 信任根。错误信息不打印目标或代理地址。
- 调用返回冷 Task；激活时通过有界阻塞池执行，取消会终止传输并释放本次连接。首版将单次响应读取到内存，不承诺跨请求连接复用或流式消费。
