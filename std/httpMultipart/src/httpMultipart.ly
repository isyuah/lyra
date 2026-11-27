import std.http.{Header, HttpError, Request}

extern fn lyraMultipartCreate(media: String, parts: Int, headers: Int, field: Int, file: Int, total: Int): CPtr;
extern fn lyraMultipartStatus(reader: CPtr): Int;
extern fn lyraMultipartEnter(reader: CPtr): Bool;
extern fn lyraMultipartLeave(reader: CPtr): Unit;
extern fn lyraMultipartFeed(reader: CPtr, bytes: Bytes): Unit;
extern fn lyraMultipartFinish(reader: CPtr): Unit;
extern fn lyraMultipartNext(reader: CPtr): Int;
extern fn lyraMultipartRead(reader: CPtr, number: Int, discard: Bool): Int;
extern fn lyraMultipartPart(reader: CPtr): CPtr;
extern fn lyraMultipartChunk(reader: CPtr): Bytes;
extern fn lyraMultipartPartNumber(part: CPtr): Int;
extern fn lyraMultipartPartName(part: CPtr): String;
extern fn lyraMultipartHasFilename(part: CPtr): Bool;
extern fn lyraMultipartFilename(part: CPtr): String;
extern fn lyraMultipartHasContentType(part: CPtr): Bool;
extern fn lyraMultipartContentType(part: CPtr): String;
extern fn lyraMultipartHeaderCount(part: CPtr): Int;
extern fn lyraMultipartHeaderName(part: CPtr, index: Int): String;
extern fn lyraMultipartHeaderValue(part: CPtr, index: Int): String;

/// 指出触发 `TooLarge` 的具体 multipart 限制。
pub enum MultipartLimit {
    /// 整个 multipart 正文的字节预算。
    TotalBytes,
    /// 表单分项总数的上限。
    PartCount,
    /// 单个分项的首部上限。
    PartHeaders,
    /// 不带 `filename` 的单个字段的字节上限。
    FieldBytes,
    /// 带 `filename` 的单个字段的字节上限。
    FileBytes,
}

/// 创建读取器、推进分项或读取分项时的失败类别。
pub enum MultipartErrorKind {
    /// 监听器未启用 `streamRequestBody`。
    StreamingRequired,
    /// 给定的分项数量或字节预算无效。
    InvalidOptions,
    /// `Content-Type` 不是含一个有效分隔边界的 `multipart/form-data`。
    ContentType,
    /// 分项首部或正文格式无效。
    Malformed,
    /// HTTP 正文在 multipart 结束边界之前耗尽。
    Truncated,
    /// 达到体积或数量限制，载荷指出是哪项预算。
    TooLarge(MultipartLimit),
    /// 当前分项还没读完或跳过，不能继续推进。
    PartPending,
    /// 该分项已失效，例如读取器已经推进到下一项。
    Closed,
    /// 同一读取器有另一个读取、推进或跳过操作。
    Busy,
    /// 读取 HTTP 请求时失败，保留原始错误。
    Http(HttpError),
}

/// 解析或传输 multipart 的结构化错误。
pub struct MultipartError(
    /// 失败类别及相应的 HTTP 错误或超限信息。
    pub val kind: MultipartErrorKind
) {
    /// 生成可读的失败详情；底层 HTTP 错误沿用自己的消息。
    pub fn message(): String {
        match kind {
            StreamingRequired => "multipart requires an HTTP request with streaming enabled",
            InvalidOptions => "invalid multipart reader limits",
            ContentType => "expected multipart/form-data with one valid boundary",
            Malformed => "malformed multipart part headers",
            Truncated => "multipart body ended before the closing boundary",
            TooLarge(limit) => match limit {
                TotalBytes => "multipart body exceeds its total byte limit",
                PartCount => "multipart body exceeds its part count limit",
                PartHeaders => "multipart part exceeds its header limit",
                FieldBytes => "multipart field exceeds its byte limit",
                FileBytes => "multipart file exceeds its byte limit",
            },
            PartPending => "finish reading or skip the current multipart part before advancing",
            Closed => "multipart part has expired",
            Busy => "another multipart reader operation is in progress",
            Http(error) => error.message(),
        }
    }
}

/// 单次 multipart 请求的数量和字节预算；跳过的字段也计入预算。
pub struct MultipartOptions(
    /// 允许出现的分项数。
    pub val maxParts: Int = 128,
    /// 单项首部字节上限，最大可配 `64 KiB`；另有固定的首部字段数限制。
    pub val maxPartHeaderBytes: Int = 16384,
    /// 不带 `filename` 的单个字段的正文上限。
    pub val maxFieldBytes: Int = 65536,
    /// 带 `filename` 的单个字段的正文上限。
    pub val maxFileBytes: Int = 8388608,
    /// 整个 multipart 正文上限，包括分隔行、首部和前后附加文字。
    pub val maxTotalBytes: Int = 8388608,
)

const inputStep: Int = 0
const partStep: Int = 1
const bytesStep: Int = 2
const endStep: Int = 3

fn multipartError(code: Int): MultipartError {
    MultipartError(match code {
        4 => MultipartErrorKind.InvalidOptions,
        5 => MultipartErrorKind.ContentType,
        6 => MultipartErrorKind.Malformed,
        7 => MultipartErrorKind.Truncated,
        8 => MultipartErrorKind.TooLarge(MultipartLimit.TotalBytes),
        9 => MultipartErrorKind.TooLarge(MultipartLimit.PartCount),
        10 => MultipartErrorKind.TooLarge(MultipartLimit.PartHeaders),
        11 => MultipartErrorKind.TooLarge(MultipartLimit.FieldBytes),
        12 => MultipartErrorKind.TooLarge(MultipartLimit.FileBytes),
        13 => MultipartErrorKind.PartPending,
        14 => MultipartErrorKind.Closed,
        _ => throw Exception("unexpected multipart parser state"),
    })
}

async fn supplyMultipartInput(handle: CPtr, request: Request): Result<Unit, MultipartError> {
    match await request.readBody() {
        Ok(Some(bytes)) => { lyraMultipartFeed(handle, bytes); Ok({}) },
        Ok(None) => { lyraMultipartFinish(handle); Ok({}) },
        Err(error) => Err(MultipartError(MultipartErrorKind.Http(error))),
    }
}

/// 按顺序解析流式表单，一次读一个分项，不缓存整份上传。
pub class MultipartReader {
    val request: Request
    val handle: CPtr

    init(request: Request, handle: CPtr) { this.request = request; this.handle = handle }

    /// 校验流式请求的媒体类型和边界，不预读正文；同一请求只创建一个读取器，不混用原始正文读取。
    pub static fn fromRequest(request: Request, options: MultipartOptions = MultipartOptions()): Result<MultipartReader, MultipartError> {
        if !request.bodyStreaming() { return Err(MultipartError(MultipartErrorKind.StreamingRequired)) }
        val media = match request.header("Content-Type") {
            Err(error) => return Err(MultipartError(MultipartErrorKind.Http(error))),
            Ok(None) => return Err(MultipartError(MultipartErrorKind.ContentType)),
            Ok(Some(value)) => value,
        }
        val handle = lyraMultipartCreate(media, options.maxParts, options.maxPartHeaderBytes,
            options.maxFieldBytes, options.maxFileBytes, options.maxTotalBytes)
        val status = lyraMultipartStatus(handle)
        if status != 0 { return Err(multipartError(status)) }
        Ok(MultipartReader(request, handle))
    }

    /// 读出下一分项的完整首部；须先读完或跳过前项。正常读完整份表单才返回 `Ok(None)`。
    pub async fn next(): Result<MultipartPart?, MultipartError> {
        if !lyraMultipartEnter(handle) { return Err(MultipartError(MultipartErrorKind.Busy)) }
        defer lyraMultipartLeave(handle)
        var step = lyraMultipartNext(handle)
        while step == inputStep {
            (await supplyMultipartInput(handle, request))?
            step = lyraMultipartNext(handle)
        }
        if step == endStep { return Ok(None) }
        if step != partStep { return Err(multipartError(step)) }
        val part = lyraMultipartPart(handle)
        val filename = if lyraMultipartHasFilename(part) { Some(lyraMultipartFilename(part)) } else { None }
        val contentType = if lyraMultipartHasContentType(part) { Some(lyraMultipartContentType(part)) } else { None }
        val headers: List<Header> = []
        for index in 0..<lyraMultipartHeaderCount(part) {
            headers.add(Header(lyraMultipartHeaderName(part, index), lyraMultipartHeaderValue(part, index)))
        }
        Ok(Some(MultipartPart(this, lyraMultipartPartNumber(part), lyraMultipartPartName(part), filename, contentType, FrozenList(headers))))
    }

    async fn readPart(number: Int, discard: Bool): Result<Bytes?, MultipartError> {
        if !lyraMultipartEnter(handle) { return Err(MultipartError(MultipartErrorKind.Busy)) }
        defer lyraMultipartLeave(handle)
        var step = lyraMultipartRead(handle, number, discard)
        while step == inputStep {
            (await supplyMultipartInput(handle, request))?
            step = lyraMultipartRead(handle, number, discard)
        }
        if step == endStep { return Ok(None) }
        if step != bytesStep { return Err(multipartError(step)) }
        Ok(Some(lyraMultipartChunk(handle)))
    }
}

/// 一个表单分项的首部及正文游标；读取器推进后不能再读该分项。
pub class MultipartPart {
    val reader: MultipartReader
    val number: Int
    /// `Content-Disposition` 中必需的字段名。
    pub val name: String
    /// 可选的客户端文件名，仅作元数据，不能直接用作保存路径。
    pub val filename: String?
    /// 分项声明的媒体类型，缺失时为 `None`。
    pub val contentType: String?
    /// 保留原始顺序和重复项的只读首部快照。
    pub val headers: FrozenList<Header>

    init(reader: MultipartReader, number: Int, name: String, filename: String?, contentType: String?, headers: FrozenList<Header>) {
        this.reader = reader
        this.number = number
        this.name = name
        this.filename = filename
        this.contentType = contentType
        this.headers = headers
    }

    /// 读取下一非空字节块；分项结束为 `Ok(None)`，不代表整份表单已经校验完成。
    pub async fn read(): Result<Bytes?, MultipartError> { await reader.readPart(number, false) }

    /// 消费并丢弃该分项的剩余正文，仍执行格式检查并计入字节预算。
    pub async fn skip(): Result<Unit, MultipartError> {
        val _ = (await reader.readPart(number, true))?
        Ok({})
    }
}
