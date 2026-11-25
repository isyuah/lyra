extern fn lyraIoReadLine(maximum: Int): CPtr;
extern fn lyraIoReadFile(path: String): CPtr;
extern fn lyraIoTextStatus(result: CPtr): Int;
extern fn lyraIoInvalidUtf8(status: Int): Bool;
extern fn lyraIoTextValue(result: CPtr): String;
extern fn lyraIoWriteFile(path: String, content: String): Int;
extern fn lyraIoWriteError(content: String): Int;
extern fn lyraIoReadBytes(path: String): CPtr;
extern fn lyraIoWriteBytes(path: String, content: Bytes): Int;
extern fn lyraIoBytesStatus(result: CPtr): Int;
extern fn lyraIoBytesValue(result: CPtr): Bytes;
extern fn lyraIoFileOpen(path: String, mode: Int): CPtr;
extern fn lyraIoFileOpenStatus(result: CPtr): Int;
extern fn lyraIoFileOpened(result: CPtr): CPtr;
extern fn lyraIoFileRead(file: CPtr, maximum: Int): CPtr;
extern fn lyraIoFileReadLine(file: CPtr, maximum: Int): CPtr;
extern fn lyraIoFileSeek(file: CPtr, offset: Int, origin: Int): CPtr;
extern fn lyraIoPositionStatus(result: CPtr): Int;
extern fn lyraIoPositionValue(result: CPtr): Int;
extern fn lyraIoFileWrite(file: CPtr, content: Bytes): Int;
extern fn lyraIoFileFlush(file: CPtr): Int;
extern fn lyraIoFileClose(file: CPtr): Int;

pub enum IoErrorKind { System(Int), Closed, Busy, Invalid, InvalidUtf8, LineTooLong }

pub struct IoError(pub val kind: IoErrorKind, pub val operation: String) {
    pub fn code(): Int? { match kind { System(code) => Some(code), _ => None } }
    pub fn message(): String {
        operation + ": " + match kind {
            System(code) => "system I/O error (code ${code})",
            Closed => "file is closed",
            Busy => "file is in use",
            Invalid => "invalid I/O argument or file mode",
            InvalidUtf8 => "input is not UTF-8",
            LineTooLong => "line exceeds its byte limit",
        }
    }
}

pub exception Busy
pub exception CloseError

fn ioError(operation: String, status: Int): IoError {
    match status {
        -2 => IoError(IoErrorKind.Closed, operation),
        -3 => IoError(IoErrorKind.Busy, operation),
        -4 => IoError(IoErrorKind.Invalid, operation),
        -5 => IoError(IoErrorKind.InvalidUtf8, operation),
        -6 => IoError(IoErrorKind.LineTooLong, operation),
        _ => IoError(if lyraIoInvalidUtf8(status) { IoErrorKind.InvalidUtf8 } else { IoErrorKind.System(status) }, operation),
    }
}

fn written(operation: String, status: Int): Result<Unit, IoError> {
    if status == 0 { Ok({}) } else { Err(ioError(operation, status)) }
}

const defaultLineBytes: Int = 1048576

fn textLine(result: CPtr): Result<String?, IoError> {
    match lyraIoTextStatus(result) {
        0 => Ok(Some(lyraIoTextValue(result))),
        -1 => Ok(None),
        status => Err(ioError("readLine", status)),
    }
}

pub fn readLine(maxBytes: Int = defaultLineBytes): Result<String?, IoError> {
    textLine(lyraIoReadLine(maxBytes))
}

pub class LineReader(val file: File?, val maxBytes: Int) {
    pub static fn stdin(maxBytes: Int = defaultLineBytes): Result<LineReader, IoError> {
        if maxBytes <= 0 { Err(IoError(IoErrorKind.Invalid, "LineReader.stdin")) }
        else { Ok(LineReader(None, maxBytes)) }
    }

    pub static fn fromFile(file: File, maxBytes: Int = defaultLineBytes): Result<LineReader, IoError> {
        if maxBytes <= 0 { Err(IoError(IoErrorKind.Invalid, "LineReader.fromFile")) }
        else { Ok(LineReader(Some(file), maxBytes)) }
    }

    pub fn readLine(): Result<String?, IoError> {
        textLine(match file {
            None => lyraIoReadLine(maxBytes),
            Some(input) => lyraIoFileReadLine(input.handle, maxBytes),
        })
    }
}

pub fn readFile(path: String): Result<String, IoError> {
    val result = lyraIoReadFile(path)
    val status = lyraIoTextStatus(result)
    if status == 0 { Ok(lyraIoTextValue(result)) } else { Err(ioError("readFile", status)) }
}

pub fn writeFile(path: String, content: String): Result<Unit, IoError> {
    written("writeFile", lyraIoWriteFile(path, content))
}

pub fn writeError(content: String): Result<Unit, IoError> {
    written("writeError", lyraIoWriteError(content))
}

pub fn readBytes(path: String): Result<Bytes, IoError> {
    val result = lyraIoReadBytes(path)
    val status = lyraIoBytesStatus(result)
    if status == 0 { Ok(lyraIoBytesValue(result)) } else { Err(ioError("readBytes", status)) }
}

pub fn writeBytes(path: String, content: Bytes): Result<Unit, IoError> {
    written("writeBytes", lyraIoWriteBytes(path, content))
}

pub enum SeekOrigin { Start, Current, End }

pub class File(val handle: CPtr) {
    pub static fn openRead(path: String): Result<File, IoError> { openFile(path, 0, "openRead") }
    pub static fn openWrite(path: String): Result<File, IoError> { openFile(path, 1, "openWrite") }
    pub static fn openAppend(path: String): Result<File, IoError> { openFile(path, 2, "openAppend") }

    pub fn read(maxBytes: Int = 65536): Result<Bytes?, IoError> {
        val result = lyraIoFileRead(handle, maxBytes)
        match lyraIoBytesStatus(result) {
            0 => Ok(Some(lyraIoBytesValue(result))),
            -1 => Ok(None),
            status => Err(ioError("read", status)),
        }
    }

    pub fn write(content: Bytes): Result<Unit, IoError> { written("write", lyraIoFileWrite(handle, content)) }
    pub fn flush(): Result<Unit, IoError> { written("flush", lyraIoFileFlush(handle)) }

    pub fn seek(offset: Int, origin: SeekOrigin = SeekOrigin.Start): Result<Int, IoError> {
        val start = match origin { Start => 0, Current => 1, End => 2 }
        val result = lyraIoFileSeek(handle, offset, start)
        val status = lyraIoPositionStatus(result)
        if status == 0 { Ok(lyraIoPositionValue(result)) } else { Err(ioError("seek", status)) }
    }

    pub fn close(): Unit {
        val status = lyraIoFileClose(handle)
        if status == 0 { return }
        val error = ioError("close", status)
        match error.kind {
            Busy => throw Busy(error.message()),
            _ => throw CloseError(error.message()),
        }
    }
}

fn openFile(path: String, mode: Int, operation: String): Result<File, IoError> {
    val result = lyraIoFileOpen(path, mode)
    val status = lyraIoFileOpenStatus(result)
    if status == 0 { Ok(File(lyraIoFileOpened(result))) } else { Err(ioError(operation, status)) }
}
