import std.compression.{GzipDecoder, GzipEncoder, GzipError, GzipErrorKind}
import std.http.{Header, HttpError, HttpErrorKind, Request, Response, ResponseWriter}
import std.sync.{AtomicInt}
import std.text.{StringOps}

extern fn lyraGzipNoTransform(field: String): Bool;

fn equalAscii(first: String, second: String): Bool {
    val left = first.toBytes()
    val right = second.toBytes()
    if left.size != right.size { return false }
    for index in 0..<left.size {
        val leftByte = lowerAscii(left[index])
        val rightByte = lowerAscii(right[index])
        if leftByte != rightByte { return false }
    }
    true
}

fn lowerAscii(byte: UInt8): Int { if 0x41 <= byte && byte <= 0x5A { byte + 0x20 } else { byte } }

fn token(text: String): Bool {
    if text == "" { return false }
    for byte in text.toBytes() {
        if (0x30 <= byte && byte <= 0x39) || (0x41 <= byte && byte <= 0x5A) || (0x61 <= byte && byte <= 0x7A) { continue }
        match byte {
            0x21 | 0x23 | 0x24 | 0x25 | 0x26 | 0x27 | 0x2A | 0x2B | 0x2D | 0x2E | 0x5E | 0x5F | 0x60 | 0x7C | 0x7E => {},
            _ => { return false },
        }
    }
    true
}

fn trimHttp(value: String): String {
    val bytes = value.toBytes()
    var start = 0
    var end = bytes.size
    while start < end && (bytes[start] == 0x20 || bytes[start] == 0x09) { start += 1 }
    while end > start && (bytes[end - 1] == 0x20 || bytes[end - 1] == 0x09) { end -= 1 }
    if start == end { return "" }
    value.substring(start, value.size - (bytes.size - end))
}

fn compressionError(error: GzipError): HttpError {
    val kind = match error.kind {
        Invalid | InputTooLarge => HttpErrorKind.Invalid,
        Closed => HttpErrorKind.Closed,
        Busy => HttpErrorKind.Busy,
        OutputTooLarge => HttpErrorKind.TooLarge,
        InvalidData | Truncated => HttpErrorKind.Malformed,
        Native(code) => HttpErrorKind.System(code),
    }
    HttpError(kind, "http.gzip", error.message())
}

fn checked<T>(result: Result<T, GzipError>): Result<T, HttpError> {
    match result { Ok(value) => Ok(value), Err(error) => Err(compressionError(error)) }
}

fn weight(input: String): Int? {
    val value = trimHttp(input).toBytes()
    if value.size == 0 || value.size > 5 || (value[0] != 0x30 && value[0] != 0x31) { return None }
    val one = value[0] == 0x31
    if value.size == 1 { return Some(if one { 1000 } else { 0 }) }
    if value[1] != 0x2E { return None }
    var result = 0
    var scale = 100
    for index in 2..<value.size {
        val digit = value[index]
        if digit < 0x30 || digit > 0x39 || (one && digit != 0x30) { return None }
        result += (digit - 0x30) * scale
        scale /= 10
    }
    Some(if one { 1000 } else { result })
}

fn minimum(previous: Int?, next: Int): Int? {
    match previous { Some(value) => Some(if value < next { value } else { next }), None => Some(next) }
}

enum Encoding { Identity, Gzip, Unacceptable }

fn preferredEncoding(fields: List<String>): Encoding {
    var gzip: Int? = None
    var wildcard: Int? = None
    var identity: Int? = None
    for field in fields {
        for item in field.split(",") {
            val value = trimHttp(item)
            if value == "" { continue }
            val parts = value.split(";")
            val coding = trimHttp(parts[0])
            if !token(coding) { return Encoding.Identity }
            var quality = 1000
            if parts.size > 2 { return Encoding.Identity }
            if parts.size == 2 {
                val parameter = parts[1].split("=")
                if parameter.size != 2 || !equalAscii(trimHttp(parameter[0]), "q") { return Encoding.Identity }
                quality = match weight(parameter[1]) { Some(parsed) => parsed, None => { return Encoding.Identity } }
            }
            if equalAscii(coding, "gzip") { gzip = minimum(gzip, quality) }
            else if equalAscii(coding, "identity") { identity = minimum(identity, quality) }
            else if coding == "*" { wildcard = minimum(wildcard, quality) }
        }
    }
    val compressed = gzip ?? wildcard ?? 0
    val original = identity ?? (if wildcard == Some(0) { 0 } else { 1000 })
    if compressed > 0 && (identity == None || compressed >= original) { Encoding.Gzip }
    else if original > 0 { Encoding.Identity }
    else { Encoding.Unacceptable }
}

fn noTransform(fields: List<String>): Bool {
    for field in fields { if lyraGzipNoTransform(field) { return true } }
    false
}

fn responseFields(response: Response, name: String): List<String> {
    val values: List<String> = []
    for field in response.headers { if equalAscii(field.name, name) { values.add(field.value) } }
    values
}

fn varied(response: Response): Response {
    var acceptEncoding = false
    var range = false
    var cacheControl = false
    for field in responseFields(response, "Vary") {
        for token in field.split(",") {
            val name = trimHttp(token)
            if name == "*" { return response }
            acceptEncoding = acceptEncoding || equalAscii(name, "Accept-Encoding")
            range = range || equalAscii(name, "Range")
            cacheControl = cacheControl || equalAscii(name, "Cache-Control")
        }
    }
    val missing: List<String> = []
    if !acceptEncoding { missing.add("Accept-Encoding") }
    if !range { missing.add("Range") }
    if !cacheControl { missing.add("Cache-Control") }
    if missing.size == 0 { return response }
    var names = missing[0]
    for index in 1..<missing.size { names += ", " + missing[index] }
    response.withHeader("Vary", names)
}

fn encodedFields(response: Response): FrozenList<Header> {
    val fields: List<Header> = []
    for field in response.headers {
        if equalAscii(field.name, "ETag") || equalAscii(field.name, "Content-MD5") || equalAscii(field.name, "Digest") ||
            equalAscii(field.name, "Content-Digest") || equalAscii(field.name, "Repr-Digest") || equalAscii(field.name, "Accept-Ranges") {
            continue
        }
        fields.add(field)
    }
    fields.add(Header("Content-Encoding", "gzip"))
    FrozenList(fields)
}

async fn writeEncoded(encoder: GzipEncoder, writer: ResponseWriter, content: Bytes): Result<Unit, HttpError> {
    if content.size == 0 { return await writer.write(content) }
    var start = 0
    while start < content.size {
        val left = content.size - start
        val end = start + (if left > 16384 { 16384 } else { left })
        val encoded = checked(encoder.pushRange(content, start, end, flush: end == content.size))?
        (await writer.write(encoded))?
        start = end
    }
    Ok({})
}

/// 按请求的 `Accept-Encoding` 协商 gzip 或原响应；两种编码均被拒绝时返回 `406`。
/// 不修改已有内容编码、范围响应和禁止转换的响应；应由调用方决定哪些正文适合压缩。
pub fn gzipResponse(request: Request, response: Response, level: Int = -1): Result<Response, HttpError> {
    if level < -1 || level > 9 { return Err(HttpError(HttpErrorKind.Invalid, "gzipResponse", "compression level must be -1 through 9")) }
    if response.code == 204 || response.code == 205 || response.code == 206 || response.code == 304 ||
        responseFields(response, "Content-Encoding").size != 0 || responseFields(response, "Content-Range").size != 0 ||
        noTransform(responseFields(response, "Cache-Control")) || request.method() == "TRACE" {
        return Ok(response)
    }
    val selected = varied(response)
    if request.headers("Range").size != 0 || noTransform(request.headers("Cache-Control")) { return Ok(selected) }
    match preferredEncoding(request.headers("Accept-Encoding")) {
        Identity => { return Ok(selected) },
        Unacceptable => { return Ok(varied(Response.text(406, "no acceptable content encoding"))) },
        Gzip => {},
    }
    val body = response.body
    val compressed = Response.stream(selected.code, selected.contentType, async (writer: ResponseWriter) => {
        val encoder = checked(GzipEncoder.open(level, maxInputChunkBytes: 16384))?
        defer { val _ = encoder.close() }
        val active = AtomicInt(1)
        val busy = AtomicInt()
        val downstream = ResponseWriter.fromSink(async (content: Bytes) => {
            if !busy.compareExchange(0, 1) { return Err(HttpError(HttpErrorKind.Busy, "gzipResponse.write")) }
            defer busy.store(0)
            if active.load() == 0 { return Err(HttpError(HttpErrorKind.Closed, "gzipResponse.write")) }
            await writeEncoded(encoder, writer, content)
        })
        defer active.store(0)
        match body {
            Buffered(content) => (await downstream.write(content))?,
            Streaming(produce) => (await produce(downstream))?,
        }
        if !busy.compareExchange(0, 1) {
            active.store(0)
            return Err(HttpError(HttpErrorKind.Busy, "gzipResponse.finish"))
        }
        defer busy.store(0)
        active.store(0)
        val tail = checked(encoder.finish())?
        (await writer.write(tail))?
        Ok({})
    })
    Ok(Response(compressed.code, compressed.contentType, compressed.body, encodedFields(selected)))
}

enum ReadState { Open, Finished, Failed(HttpError), Closed }

/// 对流式 HTTP 请求正文做增量 gzip 解压，解压预算独立于监听器的正文上限。
pub class GzipRequestReader {
    val request: Request
    val decoder: GzipDecoder
    val access: AtomicInt = AtomicInt()
    var state: ReadState = ReadState.Open

    init(request: Request, decoder: GzipDecoder) {
        this.request = request
        this.decoder = decoder
    }

    /// 要求流式请求和单一 `Content-Encoding: gzip`；同一请求只使用一个读取器，不混用 `readBody()`。
    pub static fn fromRequest(request: Request, maxOutputBytes: Int = 8388608): Result<GzipRequestReader, HttpError> {
        if !request.bodyStreaming() {
            return Err(HttpError(HttpErrorKind.Invalid, "GzipRequestReader", "enable streamRequestBody on the listener"))
        }
        val encoding = request.header("Content-Encoding")?
        if !equalAscii(trimHttp(encoding ?? ""), "gzip") {
            return Err(HttpError(HttpErrorKind.Invalid, "GzipRequestReader", "expected a single gzip content encoding"))
        }
        val decoder = checked(GzipDecoder.open(maxOutputBytes, maxInputChunkBytes: 8192))?
        Ok(GzipRequestReader(request, decoder))
    }

    /// 返回下一解压块；只有 HTTP 正文结束且 gzip 校验完成才返回 `Ok(None)`。并发读取返回 `Busy`。
    pub async fn read(): Result<Bytes?, HttpError> {
        if !access.compareExchange(0, 1) { return Err(HttpError(HttpErrorKind.Busy, "GzipRequestReader.read")) }
        defer access.store(0)
        match this.state {
            Open => {},
            Finished => { return Ok(None) },
            Failed(error) => { return Err(error) },
            Closed => { return Err(HttpError(HttpErrorKind.Closed, "GzipRequestReader.read")) },
        }
        var completed = false
        defer {
            if !completed {
                val _ = decoder.close()
                this.state = ReadState.Closed
            }
        }
        val received = await readDecoded()
        match received {
            Ok(None) => { this.state = ReadState.Finished },
            Ok(Some(_)) => {},
            Err(error) => { val _ = decoder.close(); this.state = ReadState.Failed(error) },
        }
        completed = true
        received
    }

    async fn readDecoded(): Result<Bytes?, HttpError> {
        while true {
            match checked(decoder.read())? {
                Some(content) => { return Ok(Some(content)) },
                None => {},
            }
            match (await request.readBody())? {
                Some(content) => checked(decoder.push(content))?,
                None => {
                    checked(decoder.finish())?
                    return Ok(None)
                },
            }
        }
        throw Exception("unreachable gzip reader state")
    }

    /// 放弃并关闭解码器，不关闭 HTTP 请求，也不校验尚未读到的尾部。
    pub fn close(): Result<Unit, HttpError> {
        if !access.compareExchange(0, 1) { return Err(HttpError(HttpErrorKind.Busy, "GzipRequestReader.close")) }
        defer access.store(0)
        checked(decoder.close())?
        this.state = ReadState.Closed
        Ok({})
    }
}
