extern fn lyraIoArgCount(): Int;
extern fn lyraIoArgument(index: Int): CPtr;
extern fn lyraIoReadLine(): CPtr;
extern fn lyraIoReadFile(path: String): CPtr;
extern fn lyraIoTextStatus(result: CPtr): Int;
extern fn lyraIoInvalidUtf8(status: Int): Bool;
extern fn lyraIoTextValue(result: CPtr): String;
extern fn lyraIoWriteFile(path: String, content: String): Int;
extern fn lyraIoWriteError(content: String): Int;
extern fn lyraIoAssert(condition: Bool, message: String): Unit;
extern fn lyraIoReadBytes(path: String): CPtr;
extern fn lyraIoWriteBytes(path: String, content: Bytes): Int;
extern fn lyraIoBytesStatus(result: CPtr): Int;
extern fn lyraIoBytesValue(result: CPtr): Bytes;
extern fn lyraIoFileOpen(path: String, writing: Bool): CPtr;
extern fn lyraIoFileOpenStatus(result: CPtr): Int;
extern fn lyraIoFileOpened(result: CPtr): CPtr;
extern fn lyraIoFileRead(file: CPtr, maximum: Int): CPtr;
extern fn lyraIoFileWrite(file: CPtr, content: Bytes): Int;
extern fn lyraIoFileFlush(file: CPtr): Int;
extern fn lyraIoFileClose(file: CPtr): Int;

pub struct TextRead(pub val status: Int, pub val value: String) {
    pub fn ok(): Bool { status == 0 }
    pub fn eof(): Bool { status == -1 }
    pub fn invalidUtf8(): Bool { lyraIoInvalidUtf8(status) }
}

fn text(result: CPtr): TextRead { TextRead(lyraIoTextStatus(result), lyraIoTextValue(result)) }
pub fn argCount(): Int { lyraIoArgCount() }
pub fn arg(index: Int): TextRead { text(lyraIoArgument(index)) }
pub fn readLine(): TextRead { text(lyraIoReadLine()) }
pub fn readFile(path: String): TextRead { text(lyraIoReadFile(path)) }
pub fn writeFile(path: String, content: String): Int { lyraIoWriteFile(path, content) }
pub fn writeError(content: String): Int { lyraIoWriteError(content) }
pub fn assert(condition: Bool, message: String): Unit { lyraIoAssert(condition, message) }

pub enum IoErrorKind { System(Int), Closed, Busy, Invalid }

pub struct IoError(pub val kind: IoErrorKind, pub val operation: String) {
    pub fn code(): Int? { match kind { System(errno) => Some(errno), _ => None } }
    pub fn message(): String {
        operation + ": " + match kind {
            System(errno) => "system I/O error (errno ${errno})",
            Closed => "file is closed",
            Busy => "file is in use",
            Invalid => "invalid file mode or read size",
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
        _ => IoError(IoErrorKind.System(status), operation),
    }
}

fn written(operation: String, status: Int): Result<Unit, IoError> {
    if status == 0 { Ok({}) } else { Err(ioError(operation, status)) }
}

pub fn readBytes(path: String): Result<Bytes, IoError> {
    val result = lyraIoReadBytes(path)
    val status = lyraIoBytesStatus(result)
    if status == 0 { Ok(lyraIoBytesValue(result)) } else { Err(ioError("readBytes", status)) }
}

pub fn writeBytes(path: String, content: Bytes): Result<Unit, IoError> {
    written("writeBytes", lyraIoWriteBytes(path, content))
}

pub class File(val handle: CPtr) {
    pub static fn openRead(path: String): Result<File, IoError> { openFile(path, false) }
    pub static fn openWrite(path: String): Result<File, IoError> { openFile(path, true) }

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

fn openFile(path: String, writing: Bool): Result<File, IoError> {
    val result = lyraIoFileOpen(path, writing)
    val status = lyraIoFileOpenStatus(result)
    if status == 0 { Ok(File(lyraIoFileOpened(result))) } else {
        Err(ioError(if writing { "openWrite" } else { "openRead" }, status))
    }
}
