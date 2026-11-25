extern fn lyraProcessArgs(count: Int): CPtr;
extern fn lyraProcessArg(args: CPtr, index: Int, value: String): Unit;
extern fn lyraProcessRun(args: CPtr, count: Int, cwd: String, timeoutMs: Int, maxBytes: Int): CPtr;
extern fn lyraProcessStatus(result: CPtr): Int;
extern fn lyraProcessExitCode(result: CPtr): Int;
extern fn lyraProcessSignal(result: CPtr): Int;
extern fn lyraProcessStdout(result: CPtr): Bytes;
extern fn lyraProcessStderr(result: CPtr): Bytes;

pub struct ProcessOptions(pub val timeoutMs: Int = 30000, pub val maxOutputBytes: Int = 1048576, pub val cwd: String? = None)
pub struct ProcessResult(pub val exitCode: Int, pub val signal: Int?, pub val stdout: Bytes, pub val stderr: Bytes)
pub enum ProcessError { Invalid, Timeout, OutputLimit, System(Int) }

async fn collectProcess(arguments: CPtr, count: Int, options: ProcessOptions): Result<ProcessResult, ProcessError> {
    val result = await blocking { lyraProcessRun(arguments, count, options.cwd ?? "", options.timeoutMs, options.maxOutputBytes) }
    match lyraProcessStatus(result) {
        0 => {
            val signal = lyraProcessSignal(result)
            Ok(ProcessResult(lyraProcessExitCode(result), if signal == 0 { None } else { Some(signal) }, lyraProcessStdout(result), lyraProcessStderr(result)))
        },
        -1 => Err(ProcessError.Invalid),
        -2 => Err(ProcessError.Timeout),
        -3 => Err(ProcessError.OutputLimit),
        status => Err(ProcessError.System(status)),
    }
}

pub async fn runProcess(argv: List<String>, options: ProcessOptions = ProcessOptions()): Result<ProcessResult, ProcessError> {
    if options.timeoutMs < 1 || options.timeoutMs > 604800000 || options.maxOutputBytes < 1 || options.maxOutputBytes > 67108864 {
        return Err(ProcessError.Invalid)
    }
    val snapshot = FrozenList(argv)
    val count = snapshot.size
    if count == 0 { return Err(ProcessError.Invalid) }
    val arguments = lyraProcessArgs(count)
    for index in 0..<count { lyraProcessArg(arguments, index, snapshot[index]) }
    val results = Channel<Result<ProcessResult, ProcessError>>(1)
    val worker = spawn async { await results.send(await collectProcess(arguments, count, options)) }
    await select {
        val received = results.receive() => received ?? throw Exception("process result channel closed unexpectedly"),
        sleepMs(options.timeoutMs) => {
            worker.cancel()
            val _ = awaitOutcome(worker)
            Err(ProcessError.Timeout)
        },
    }
}
