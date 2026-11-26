# `std.httpMultipart`

在 `HttpServerOptions(streamRequestBody: true)` 的 handler 中，通过 `MultipartReader.fromRequest` 读取 `multipart/form-data`。创建 reader 会校验 `Content-Type` 和边界，不读正文。

```lyra
import std.http.{Request}
import std.httpMultipart.{MultipartError, MultipartOptions, MultipartReader}

async fn countUpload(request: Request): Result<Int, MultipartError> {
    val reader = MultipartReader.fromRequest(request, MultipartOptions(maxFileBytes: 2 * 1024 * 1024))?
    var bytes = 0
    while true {
        match (await reader.next())? {
            None => break,
            Some(part) => {
                if part.name != "upload" { (await part.skip())?; continue }
                while true {
                    match (await part.read())? {
                        None => break,
                        Some(chunk) => { bytes += chunk.size },
                    }
                }
            },
        }
    }
    Ok(bytes)
}
```

- `next()` 在当前 part 的完整首部就绪后返回；`read()` 返回非空二进制块，part 结束为 `Ok(None)`。下一次 `next()` 前须读完或 `skip()`，否则返回 `PartPending`；推进后旧 part 返回 `Closed`。同一个 reader 同时只允许一个读、跳过或推进操作，竞争返回 `Busy`。
- 一个请求只创建一个 reader，不混用 `Request.readBody()`。reader 只在需要字节时向 HTTP 请求读取；不把文件或整份表单留在内存。挂起读取取消沿用请求的连接清理规则。
- `MultipartPart` 提供 `name`、`filename`、`contentType` 和保留原始顺序及重复项的只读 `headers`。`Content-Disposition: form-data` 必须附带 `name`；允许引号和反斜杠转义。首部须为 UTF-8，且用 CRLF 分行；不支持折行、`filename*`、`Content-Transfer-Encoding` 或递归拆解嵌套 MIME 正文。
- `filename` 只是不可信的元数据。保存文件时使用服务端决定的路径；可用 `std.io.createTempFile` 接收内容，保证失败或取消时关闭并清理，直到 `reader.next()` 返回 `Ok(None)` 再提交。单个 part 读完不代表整份上传成功。
- `MultipartOptions` 分别限制 part 数（默认 128）、每个 part 的首部字节（默认 16 KiB，最多配置 64 KiB，固定最多 64 个首部字段）、普通字段（64 KiB）、带 `filename` 的字段（8 MiB）和总正文（8 MiB）。`skip()` 也计入预算。`TooLarge` 携带触及上限的 `MultipartLimit`。
- 解析支持跨任意读块的边界、带引号的 boundary、标准 preamble/epilogue，接受最多 1 KiB 的分隔行尾部空格或制表符。总量包含分隔行、首部、preamble 和 epilogue，不包括外层 HTTP chunked framing。完成时读取至 HTTP EOF，使正常连接可以复用。
