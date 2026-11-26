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

pub enum MultipartLimit { TotalBytes, PartCount, PartHeaders, FieldBytes, FileBytes }

pub enum MultipartErrorKind {
    StreamingRequired, InvalidOptions, ContentType, Malformed, Truncated, TooLarge(MultipartLimit), PartPending, Closed, Busy, Http(HttpError),
}

pub struct MultipartError(pub val kind: MultipartErrorKind) {
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

pub struct MultipartOptions(
    pub val maxParts: Int = 128,
    pub val maxPartHeaderBytes: Int = 16384,
    pub val maxFieldBytes: Int = 65536,
    pub val maxFileBytes: Int = 8388608,
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

pub class MultipartReader {
    val request: Request
    val handle: CPtr

    init(request: Request, handle: CPtr) { this.request = request; this.handle = handle }

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

pub class MultipartPart {
    val reader: MultipartReader
    val number: Int
    pub val name: String
    pub val filename: String?
    pub val contentType: String?
    pub val headers: FrozenList<Header>

    init(reader: MultipartReader, number: Int, name: String, filename: String?, contentType: String?, headers: FrozenList<Header>) {
        this.reader = reader
        this.number = number
        this.name = name
        this.filename = filename
        this.contentType = contentType
        this.headers = headers
    }

    pub async fn read(): Result<Bytes?, MultipartError> { await reader.readPart(number, false) }

    pub async fn skip(): Result<Unit, MultipartError> {
        val _ = (await reader.readPart(number, true))?
        Ok({})
    }
}
