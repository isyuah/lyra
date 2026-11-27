extern fn lyraContextCurrent(): CPtr;
extern fn lyraContextWithTrace(context: CPtr, trace: String): CPtr;
extern fn lyraContextWithTimeout(context: CPtr, milliseconds: Int): CPtr;
extern fn lyraContextTrace(context: CPtr): String;
extern fn lyraContextRemaining(context: CPtr): Int;
extern fn lyraContextEnter(context: CPtr): Bool;
extern fn lyraTaskParallelism(): Int;

/// 读取当前任务作用域的追踪标识；根作用域未设置时返回空字符串。
pub fn trace(): String { lyraContextTrace(lyraContextCurrent()) }
/// 返回有效截止时间剩余的单调时钟毫秒数；无截止时间返回 `-1`，已到期返回 `0`。
pub fn remainingMs(): Int { lyraContextRemaining(lyraContextCurrent()) }
/// 返回并行工作线程池容量，不含根执行线程；由 `LYRA_PARALLEL_WORKERS` 配置，默认 `2`。
pub fn parallelism(): Int { lyraTaskParallelism() }

/// 在带有 `label` 的子上下文中等待 `body`；返回冷任务，不改写调用方的追踪标识。
pub async fn withTrace<T>(label: String, body: Task<T>): T {
    lyraContextEnter(lyraContextWithTrace(lyraContextCurrent(), label))
    await body
}

/// 在不晚于父作用域截止时间的期限内等待 `body`。非正超时立即请求取消；已在别处启动的任务只会停止本次等待。
pub async fn withTimeout<T>(milliseconds: Int, body: Task<T>): T {
    lyraContextEnter(lyraContextWithTimeout(lyraContextCurrent(), milliseconds))
    await body
}
