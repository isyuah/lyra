extern fn lyraGzipEncoderOpen(level: Int, maxInputChunkBytes: Int, maxOutputChunkBytes: Int): CPtr;
extern fn lyraGzipEncode(encoder: CPtr, content: Bytes, flush: Bool): CPtr;
extern fn lyraGzipEncodeRange(encoder: CPtr, content: Bytes, start: Int, end: Int, flush: Bool): CPtr;
extern fn lyraGzipEncoderFinish(encoder: CPtr): CPtr;
extern fn lyraGzipEncoderClose(encoder: CPtr): Int;
extern fn lyraGzipDecoderOpen(maxOutputBytes: Int, maxInputChunkBytes: Int, outputChunkBytes: Int): CPtr;
extern fn lyraGzipDecoderPush(decoder: CPtr, content: Bytes): Int;
extern fn lyraGzipDecoderRead(decoder: CPtr): CPtr;
extern fn lyraGzipDecoderFinish(decoder: CPtr): Int;
extern fn lyraGzipDecoderClose(decoder: CPtr): Int;
extern fn lyraGzipResultStatus(result: CPtr): Int;
extern fn lyraGzipResultCode(result: CPtr): Int;
extern fn lyraGzipResultHandle(result: CPtr): CPtr;
extern fn lyraGzipResultBytes(result: CPtr): Bytes;
extern fn lyraGzipResultHasBytes(result: CPtr): Bool;

/// 增量 gzip 压缩或解压失败的类别。
pub enum GzipErrorKind {
    /// 选项或本次操作参数无效。
    Invalid,
    /// 流已经结束、显式关闭或因错误关闭。
    Closed,
    /// 另一个调用正在使用流，或解码器仍有输入待读。
    Busy,
    /// 单次输入超过分块上限，输入未被消费。
    InputTooLarge,
    /// 压缩单次输出或解压累计输出超过配置上限，流会关闭。
    OutputTooLarge,
    /// gzip 首部、压缩数据、CRC 或原始大小无效。
    InvalidData,
    /// 源输入结束时，当前 gzip 成员还没完整结束。
    Truncated,
    /// 底层 zlib 操作失败，载荷为原生状态码。
    Native(Int),
}

/// 包含失败分类和发生错误的 gzip 操作。
pub struct GzipError(
    /// 用于判断是否需先读取、修正参数或丢弃当前流的错误类型。
    pub val kind: GzipErrorKind,
    /// 失败的操作名，例如 `gzip.decoder.finish`。
    pub val operation: String
) {
    /// 格式化操作名和失败原因。
    pub fn message(): String {
        operation + ": " + match kind {
            Invalid => "invalid gzip options",
            Closed => "gzip stream is closed",
            Busy => "gzip stream is in use or has input left to drain",
            InputTooLarge => "gzip input exceeds its chunk limit",
            OutputTooLarge => "gzip output exceeds its byte limit",
            InvalidData => "invalid gzip header, compressed data, or checksum",
            Truncated => "gzip input ended without a complete member",
            Native(code) => "gzip codec failed (native code ${code})",
        }
    }
}

fn gzipError(operation: String, status: Int, code: Int = 0): GzipError {
    val kind = match status {
        1 => GzipErrorKind.Invalid,
        2 => GzipErrorKind.Closed,
        3 => GzipErrorKind.Busy,
        4 => GzipErrorKind.InputTooLarge,
        5 => GzipErrorKind.OutputTooLarge,
        6 => GzipErrorKind.InvalidData,
        7 => GzipErrorKind.Truncated,
        8 => GzipErrorKind.Native(code),
        _ => throw Exception("unexpected native gzip status"),
    }
    GzipError(kind, operation)
}

fn accepted(operation: String, status: Int): Result<Unit, GzipError> {
    if status == 0 { Ok({}) } else { Err(gzipError(operation, status)) }
}

fn checkedResult(operation: String, result: CPtr): Result<CPtr, GzipError> {
    val status = lyraGzipResultStatus(result)
    if status == 0 { Ok(result) } else { Err(gzipError(operation, status, lyraGzipResultCode(result))) }
}

/// 按顺序把字节块压缩成一个 gzip 成员；调用方须依次保存每次返回的字节。
pub class GzipEncoder {
    val handle: CPtr
    init(handle: CPtr) { this.handle = handle }

    /// 创建编码器，级别 `-1` 使用 zlib 默认值，`0`–`9` 指定级别；输入和输出上限按单次调用计算。
    pub static fn open(level: Int = -1, maxInputChunkBytes: Int = 65536, maxOutputChunkBytes: Int = 262144): Result<GzipEncoder, GzipError> {
        val result = checkedResult("gzip.encoder.open", lyraGzipEncoderOpen(level, maxInputChunkBytes, maxOutputChunkBytes))?
        Ok(GzipEncoder(lyraGzipResultHandle(result)))
    }

    /// 消费完整输入块并返回当前可用的压缩字节，返回空块属正常；`flush` 刷出可解码的数据但不结束成员。
    pub fn push(content: Bytes, flush: Bool = false): Result<Bytes, GzipError> {
        val result = checkedResult("gzip.encoder.push", lyraGzipEncode(handle, content, flush))?
        Ok(lyraGzipResultBytes(result))
    }

    /// 编码 `[start, end)` 字节区间，无需先复制切片；该区间受与 `push` 相同的限制。
    pub fn pushRange(content: Bytes, start: Int, end: Int, flush: Bool = false): Result<Bytes, GzipError> {
        val result = checkedResult("gzip.encoder.pushRange", lyraGzipEncodeRange(handle, content, start, end, flush))?
        Ok(lyraGzipResultBytes(result))
    }

    /// 写出剩余压缩数据和 gzip 尾部并关闭编码器；返回字节应接在先前输出之后。
    pub fn finish(): Result<Bytes, GzipError> {
        val result = checkedResult("gzip.encoder.finish", lyraGzipEncoderFinish(handle))?
        Ok(lyraGzipResultBytes(result))
    }

    /// 释放编码器，不写尾部；已关闭时可再次调用。
    pub fn close(): Result<Unit, GzipError> { accepted("gzip.encoder.close", lyraGzipEncoderClose(handle)) }
}

/// 增量解压一个或多个拼接的 gzip 成员，所有成员共享一个解压字节预算。
pub class GzipDecoder {
    val handle: CPtr
    init(handle: CPtr) { this.handle = handle }

    /// 设置累计解压上限和单次输入、输出块大小；累计上限为 `0` 时只允许空输出。
    pub static fn open(maxOutputBytes: Int = 8388608, maxInputChunkBytes: Int = 65536, outputChunkBytes: Int = 16384): Result<GzipDecoder, GzipError> {
        val result = checkedResult("gzip.decoder.open", lyraGzipDecoderOpen(maxOutputBytes, maxInputChunkBytes, outputChunkBytes))?
        Ok(GzipDecoder(lyraGzipResultHandle(result)))
    }

    /// 提交一个压缩输入块；再次提交前须调用 `read()` 直到它返回 `Ok(None)`。
    pub fn push(content: Bytes): Result<Unit, GzipError> { accepted("gzip.decoder.push", lyraGzipDecoderPush(handle, content)) }

    /// 取一块解压数据；`Ok(None)` 表示需要更多输入，不表示流已完整校验。
    pub fn read(): Result<Bytes?, GzipError> {
        val result = checkedResult("gzip.decoder.read", lyraGzipDecoderRead(handle))?
        Ok(if lyraGzipResultHasBytes(result) { Some(lyraGzipResultBytes(result)) } else { None })
    }

    /// 在源 EOF 且输出取尽后调用，确认 gzip 成员完整并关闭解码器。
    pub fn finish(): Result<Unit, GzipError> { accepted("gzip.decoder.finish", lyraGzipDecoderFinish(handle)) }
    /// 放弃解码并释放资源，不验证尾部；可重复关闭。
    pub fn close(): Result<Unit, GzipError> { accepted("gzip.decoder.close", lyraGzipDecoderClose(handle)) }
}
