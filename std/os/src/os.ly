import std.io.{IoError, IoErrorKind}

extern fn lyraIoArgCount(): Int;
extern fn lyraIoArgument(index: Int): CPtr;
extern fn lyraIoTextStatus(result: CPtr): Int;
extern fn lyraIoInvalidUtf8(status: Int): Bool;
extern fn lyraIoTextValue(result: CPtr): String;
extern fn lyraOsEnv(name: String): CPtr;
extern fn lyraOsWindows(): Bool;
extern fn lyraOsWaitSignal(kind: Int): Task<Int>;

/// 当前运行时支持的宿主操作系统。
pub enum Platform {
    /// Windows 宿主。
    Windows,
    /// Linux 宿主。
    Linux,
}
/// 目标 CPU 架构的编译期常量，当前为 `x86_64`。
pub const architecture: String = "x86_64"
/// 返回当前运行程序的宿主操作系统。
pub fn platform(): Platform { if lyraOsWindows() { Platform.Windows } else { Platform.Linux } }

/// 可通过 `waitSignal` 等待的进程信号，具体支持情况取决于平台。
pub enum Signal {
    /// Linux 的 SIGINT 或 Windows 控制台的 Ctrl+C。
    Interrupt,
    /// Linux 的 SIGTERM；Windows 不支持。
    Terminate,
    /// Windows 控制台的 Ctrl+Break；Linux 不支持。
    Break,
}

/// 订阅信号失败的原因。
pub enum SignalError {
    /// 当前平台不支持该信号。
    Unsupported,
    /// 原生订阅操作失败，保留平台错误码。
    System(Int),
}

/// 返回等待一次信号的冷任务；任务启动时订阅，收到信号或取消后移除监听。
pub async fn waitSignal(signal: Signal): Result<Unit, SignalError> {
    val status = await lyraOsWaitSignal(match signal { Interrupt => 0, Terminate => 1, Break => 2 })
    match status { 0 => Ok({}), -1 => Err(SignalError.Unsupported), _ => Err(SignalError.System(status)) }
}

fn osError(operation: String, status: Int): IoError {
    if status == -4 { return IoError(IoErrorKind.Invalid, operation) }
    if lyraIoInvalidUtf8(status) { return IoError(IoErrorKind.InvalidUtf8, operation) }
    IoError(IoErrorKind.System(status), operation)
}

/// 读取进程参数，不含程序名，保留参数顺序、边界和空字符串；不能解码时返回 `InvalidUtf8`。
pub fn args(): Result<List<String>, IoError> {
    val result: List<String> = []
    for index in 0..<lyraIoArgCount() {
        val argument = lyraIoArgument(index)
        val status = lyraIoTextStatus(argument)
        if status != 0 { return Err(osError("args", status)) }
        result.add(lyraIoTextValue(argument))
    }
    Ok(result)
}

/// 查询环境变量：未设置是 `None`，已设置为空是 `Some("")`；名称不得为空或含 `=`、NUL。
pub fn env(name: String): Result<String?, IoError> {
    val value = lyraOsEnv(name)
    match lyraIoTextStatus(value) {
        0 => Ok(Some(lyraIoTextValue(value))),
        -1 => Ok(None),
        status => Err(osError("env", status)),
    }
}
