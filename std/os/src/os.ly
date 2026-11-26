import std.io.{IoError, IoErrorKind}

extern fn lyraIoArgCount(): Int;
extern fn lyraIoArgument(index: Int): CPtr;
extern fn lyraIoTextStatus(result: CPtr): Int;
extern fn lyraIoInvalidUtf8(status: Int): Bool;
extern fn lyraIoTextValue(result: CPtr): String;
extern fn lyraOsEnv(name: String): CPtr;
extern fn lyraOsWindows(): Bool;
extern fn lyraOsWaitSignal(kind: Int): Task<Int>;

pub enum Platform { Windows, Linux }
pub const architecture: String = "x86_64"
pub fn platform(): Platform { if lyraOsWindows() { Platform.Windows } else { Platform.Linux } }

pub enum Signal { Interrupt, Terminate, Break }
pub enum SignalError { Unsupported, System(Int) }

pub async fn waitSignal(signal: Signal): Result<Unit, SignalError> {
    val status = await lyraOsWaitSignal(match signal { Interrupt => 0, Terminate => 1, Break => 2 })
    match status { 0 => Ok({}), -1 => Err(SignalError.Unsupported), _ => Err(SignalError.System(status)) }
}

fn osError(operation: String, status: Int): IoError {
    if status == -4 { return IoError(IoErrorKind.Invalid, operation) }
    if lyraIoInvalidUtf8(status) { return IoError(IoErrorKind.InvalidUtf8, operation) }
    IoError(IoErrorKind.System(status), operation)
}

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

pub fn env(name: String): Result<String?, IoError> {
    val value = lyraOsEnv(name)
    match lyraIoTextStatus(value) {
        0 => Ok(Some(lyraIoTextValue(value))),
        -1 => Ok(None),
        status => Err(osError("env", status)),
    }
}
