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

pub enum GzipErrorKind { Invalid, Closed, Busy, InputTooLarge, OutputTooLarge, InvalidData, Truncated, Native(Int) }

pub struct GzipError(pub val kind: GzipErrorKind, pub val operation: String) {
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

pub class GzipEncoder {
    val handle: CPtr
    init(handle: CPtr) { this.handle = handle }

    pub static fn open(level: Int = -1, maxInputChunkBytes: Int = 65536, maxOutputChunkBytes: Int = 262144): Result<GzipEncoder, GzipError> {
        val result = checkedResult("gzip.encoder.open", lyraGzipEncoderOpen(level, maxInputChunkBytes, maxOutputChunkBytes))?
        Ok(GzipEncoder(lyraGzipResultHandle(result)))
    }

    pub fn push(content: Bytes, flush: Bool = false): Result<Bytes, GzipError> {
        val result = checkedResult("gzip.encoder.push", lyraGzipEncode(handle, content, flush))?
        Ok(lyraGzipResultBytes(result))
    }

    pub fn pushRange(content: Bytes, start: Int, end: Int, flush: Bool = false): Result<Bytes, GzipError> {
        val result = checkedResult("gzip.encoder.pushRange", lyraGzipEncodeRange(handle, content, start, end, flush))?
        Ok(lyraGzipResultBytes(result))
    }

    pub fn finish(): Result<Bytes, GzipError> {
        val result = checkedResult("gzip.encoder.finish", lyraGzipEncoderFinish(handle))?
        Ok(lyraGzipResultBytes(result))
    }

    pub fn close(): Result<Unit, GzipError> { accepted("gzip.encoder.close", lyraGzipEncoderClose(handle)) }
}

pub class GzipDecoder {
    val handle: CPtr
    init(handle: CPtr) { this.handle = handle }

    pub static fn open(maxOutputBytes: Int = 8388608, maxInputChunkBytes: Int = 65536, outputChunkBytes: Int = 16384): Result<GzipDecoder, GzipError> {
        val result = checkedResult("gzip.decoder.open", lyraGzipDecoderOpen(maxOutputBytes, maxInputChunkBytes, outputChunkBytes))?
        Ok(GzipDecoder(lyraGzipResultHandle(result)))
    }

    pub fn push(content: Bytes): Result<Unit, GzipError> { accepted("gzip.decoder.push", lyraGzipDecoderPush(handle, content)) }

    pub fn read(): Result<Bytes?, GzipError> {
        val result = checkedResult("gzip.decoder.read", lyraGzipDecoderRead(handle))?
        Ok(if lyraGzipResultHasBytes(result) { Some(lyraGzipResultBytes(result)) } else { None })
    }

    pub fn finish(): Result<Unit, GzipError> { accepted("gzip.decoder.finish", lyraGzipDecoderFinish(handle)) }
    pub fn close(): Result<Unit, GzipError> { accepted("gzip.decoder.close", lyraGzipDecoderClose(handle)) }
}
