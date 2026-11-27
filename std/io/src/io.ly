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

/// I/O 操作失败的类别；文件结尾用 `Ok(None)` 表示，不是错误。
pub enum IoErrorKind {
    /// 原生系统 I/O 错误，载荷是平台相关错误码。
    System(Int),
    /// 文件句柄已经关闭。
    Closed,
    /// 同一文件正在被其他操作使用。
    Busy,
    /// 参数或打开模式不适用于当前操作。
    Invalid,
    /// 输入无法解码成有效 UTF-8。
    InvalidUtf8,
    /// 一行的正文超过允许的字节数。
    LineTooLong,
}

/// 结构化 I/O 错误，包含失败类型和操作名。
pub struct IoError(
    /// 可用于匹配和恢复的错误类别。
    pub val kind: IoErrorKind,
    /// 报告错误的操作名，例如 `readLine` 或 `close`。
    pub val operation: String
) {
    /// 仅对 `System` 返回原生错误码；编码取决于宿主平台。
    pub fn code(): Int? { match kind { System(code) => Some(code), _ => None } }
    /// 格式化操作名与错误原因。
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

/// 关闭文件时有操作占用句柄；操作完成后可以重试关闭。
pub exception Busy
/// 关闭文件时发生非占用类 I/O 错误。
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

/// 从 stdin 同步读取一行 UTF-8；保留空行，EOF 返回 `None`，正文最多 `maxBytes` 字节。
/// 不计 LF 或 CRLF 行尾；无效或超长的行仍会被消费。
pub fn readLine(maxBytes: Int = defaultLineBytes): Result<String?, IoError> {
    textLine(lyraIoReadLine(maxBytes))
}

/// 带固定字节上限的 UTF-8 行读取器，借用输入而不负责关闭。
pub class LineReader(val file: File?, val maxBytes: Int) {
    /// 从共享 stdin 读取，`maxBytes` 必须为正数。
    pub static fn stdin(maxBytes: Int = defaultLineBytes): Result<LineReader, IoError> {
        if maxBytes <= 0 { Err(IoError(IoErrorKind.Invalid, "LineReader.stdin")) }
        else { Ok(LineReader(None, maxBytes)) }
    }

    /// 借用已有文件，从共享的当前位置读取；`maxBytes` 必须为正数。
    pub static fn fromFile(file: File, maxBytes: Int = defaultLineBytes): Result<LineReader, IoError> {
        if maxBytes <= 0 { Err(IoError(IoErrorKind.Invalid, "LineReader.fromFile")) }
        else { Ok(LineReader(Some(file), maxBytes)) }
    }

    /// 读取下一行，不含 LF/CRLF；EOF 为 `None`，空行是 `Some("")`，坏行会被完整消费。
    pub fn readLine(): Result<String?, IoError> {
        textLine(match file {
            None => lyraIoReadLine(maxBytes),
            Some(input) => lyraIoFileReadLine(input.handle, maxBytes),
        })
    }
}

/// 读取整个文件并校验 UTF-8；空文件得到空字符串，保留内嵌 NUL。
pub fn readFile(path: String): Result<String, IoError> {
    val result = lyraIoReadFile(path)
    val status = lyraIoTextStatus(result)
    if status == 0 { Ok(lyraIoTextValue(result)) } else { Err(ioError("readFile", status)) }
}

/// 创建或截断文件并写入完整 UTF-8 文本；写入或关闭失败都会返回错误。
pub fn writeFile(path: String, content: String): Result<Unit, IoError> {
    written("writeFile", lyraIoWriteFile(path, content))
}

/// 原样写入并刷新 stderr，不自动添加换行。
pub fn writeError(content: String): Result<Unit, IoError> {
    written("writeError", lyraIoWriteError(content))
}

/// 读取整个文件的原始字节，不做 UTF-8 校验或换行转换。
pub fn readBytes(path: String): Result<Bytes, IoError> {
    val result = lyraIoReadBytes(path)
    val status = lyraIoBytesStatus(result)
    if status == 0 { Ok(lyraIoBytesValue(result)) } else { Err(ioError("readBytes", status)) }
}

/// 创建或截断文件并原样写入字节；报告写入或关闭失败。
pub fn writeBytes(path: String, content: Bytes): Result<Unit, IoError> {
    written("writeBytes", lyraIoWriteBytes(path, content))
}

/// `File.seek` 计算带符号字节偏移时的基准位置。
pub enum SeekOrigin {
    /// 以文件开头为基准。
    Start,
    /// 以当前文件位置为基准。
    Current,
    /// 以文件末尾为基准，向前定位时使用负偏移。
    End,
}

/// 同步文件句柄，多个引用共享位置和关闭状态；使用 `withRead` 等方法可自动清理。
pub class File(val handle: CPtr) {
    /// 打开已存在的文件供二进制读取。
    pub static fn openRead(path: String): Result<File, IoError> { openFile(path, 0, "openRead") }
    /// 打开文件供写入；不存在则创建，已存在则截断。
    pub static fn openWrite(path: String): Result<File, IoError> { openFile(path, 1, "openWrite") }
    /// 创建或打开文件供追加，写入始终发生在文件末尾。
    pub static fn openAppend(path: String): Result<File, IoError> { openFile(path, 2, "openAppend") }

    /// 打开只读文件交给回调，回调返回或抛异常时都会关闭文件。
    pub static fn withRead<T>(path: String, body: (File) -> Result<T, IoError>): Result<T, IoError> { openAndUse<T>(path, 0, "openRead", body) }
    /// 创建或截断文件后调用写入回调，离开时关闭句柄，返回回调结果。
    pub static fn withWrite<T>(path: String, body: (File) -> Result<T, IoError>): Result<T, IoError> { openAndUse<T>(path, 1, "openWrite", body) }
    /// 以追加模式调用回调，离开时关闭句柄，保留已有文件内容。
    pub static fn withAppend<T>(path: String, body: (File) -> Result<T, IoError>): Result<T, IoError> { openAndUse<T>(path, 2, "openAppend", body) }

    /// 从当前位置读取最多 `maxBytes` 字节；读取结果非空，EOF 返回 `Ok(None)`。
    pub fn read(maxBytes: Int = 65536): Result<Bytes?, IoError> {
        val result = lyraIoFileRead(handle, maxBytes)
        match lyraIoBytesStatus(result) {
            0 => Ok(Some(lyraIoBytesValue(result))),
            -1 => Ok(None),
            status => Err(ioError("read", status)),
        }
    }

    /// 写入原始字节；追加模式下写到末尾，与此前的定位无关。
    pub fn write(content: Bytes): Result<Unit, IoError> { written("write", lyraIoFileWrite(handle, content)) }
    /// 刷新输出缓冲并报告写入失败，不承诺数据已持久化到磁盘。
    pub fn flush(): Result<Unit, IoError> { written("flush", lyraIoFileFlush(handle)) }

    /// 按 `origin` 和有符号字节偏移定位，返回从文件开头计算的新位置。
    pub fn seek(offset: Int, origin: SeekOrigin = SeekOrigin.Start): Result<Int, IoError> {
        val start = match origin { Start => 0, Current => 1, End => 2 }
        val result = lyraIoFileSeek(handle, offset, start)
        val status = lyraIoPositionStatus(result)
        if status == 0 { Ok(lyraIoPositionValue(result)) } else { Err(ioError("seek", status)) }
    }

    /// 关闭所有别名所指向的句柄；重复关闭无效，被占用时抛 `Busy`，其他失败抛 `CloseError`。
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

fn openAndUse<T>(path: String, mode: Int, operation: String, body: (File) -> Result<T, IoError>): Result<T, IoError> {
    val file = openFile(path, mode, operation)?
    defer file.close()
    body(file)
}

fn openFile(path: String, mode: Int, operation: String): Result<File, IoError> {
    val result = lyraIoFileOpen(path, mode)
    val status = lyraIoFileOpenStatus(result)
    if status == 0 { Ok(File(lyraIoFileOpened(result))) } else { Err(ioError(operation, status)) }
}
