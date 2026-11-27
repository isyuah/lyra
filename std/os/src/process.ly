extern fn lyraProcessArgs(count: Int): CPtr;
extern fn lyraProcessArg(args: CPtr, index: Int, value: String): Unit;
extern fn lyraProcessRun(args: CPtr, count: Int, cwd: String, timeoutMs: Int, maxBytes: Int, input: Bytes): CPtr;
extern fn lyraProcessStatus(result: CPtr): Int;
extern fn lyraProcessExitCode(result: CPtr): Int;
extern fn lyraProcessSignal(result: CPtr): Int;
extern fn lyraProcessStdout(result: CPtr): Bytes;
extern fn lyraProcessStderr(result: CPtr): Bytes;

/// 子进程的运行时限、I/O 预算和运行目录。
pub struct ProcessOptions(
    /// 从任务启动起计算的毫秒数，含阻塞池排队时间；允许 `1` 毫秒至 `7` 天。
    pub val timeoutMs: Int = 30000,
    /// stdout 与 stderr 合计最多捕获的字节数，允许 `1` 字节至 `64 MiB`。
    pub val maxOutputBytes: Int = 1048576,
    /// 子进程工作目录；未设置时继承调用进程的工作目录。
    pub val cwd: String? = None,
    /// 发送到 stdin 的字节；写完即关闭输入，缺省不发送数据。
    pub val stdin: Bytes = b""
)

/// 直接子进程的结束状态和捕获的输出。非零退出码仍是一个结果。
pub struct ProcessResult(
    /// 子进程报告的退出码；若由信号结束，请同时检查 `signal`。
    pub val exitCode: Int,
    /// 结束进程的原生信号号，正常退出时为 `None`。
    pub val signal: Int?,
    /// stdout 的原始字节，不做文本解码。
    pub val stdout: Bytes,
    /// stderr 的原始字节，不做文本解码。
    pub val stderr: Bytes
)

/// 子进程启动、管道传输或资源限制导致的失败。
pub enum ProcessError {
    /// 执行参数或配置不合法，例如参数列表为空或输出上限超范围。
    Invalid,
    /// 超过运行时限，已启动的直接子进程会被终止并回收。
    Timeout,
    /// 两个输出流合计超过字节预算，不返回截断后的结果。
    OutputLimit,
    /// 启动或 I/O 等原生操作失败，载荷为原生错误码。
    System(Int),
}

async fn collectProcess(arguments: CPtr, count: Int, options: ProcessOptions): Result<ProcessResult, ProcessError> {
    val result = await blocking { lyraProcessRun(arguments, count, options.cwd ?? "", options.timeoutMs, options.maxOutputBytes, options.stdin) }
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

/// 启动时对 `argv` 取快照，首项是程序，余项按独立参数传递，不经过 shell。
/// 继承环境，捕获 stdout 和 stderr；超时、取消或超限时回收直接子进程，不管理后代进程。
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
