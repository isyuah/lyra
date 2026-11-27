# std.task：不用逐层传 context

```text
import std.task.{trace}
import std.task.{withTrace}
import std.task.{withTimeout}

async fn query(): String { await blocking { trace() } }
async fn main(): Unit {
    val cold = query()
    val label = await withTrace("request-7", withTimeout(5000, cold))
    print(label)
}
```

`withTrace(label, task)` 和 `withTimeout(milliseconds, task)` 返回冷 Task。第一次 `await` 或 `spawn` 才创建局部上下文，并在该作用域内激活传入的任务；离开后调用方的上下文保持原样。`trace()` 返回当前追踪标识（根为 `""`）。阻塞池中的同步代码也能读取属于本次任务的快照，线程复用不混用标识。

`remainingMs()` 返回当前有效截止时间的剩余毫秒；没有时为 `-1`，已到期为 `0`。时间用单调时钟计算，子作用域只可缩短期限，非正超时立即请求取消。到期取消面对的是当前任务树；如果传入的是**已经由另一个作用域启动**的任务，只终止这里的等待。取消不强停同步 C 调用，已启动的工作必须安全收尾。

编译器在 Lyra 循环回边和递归调用处自动检查取消，包括同步调用及 `blocking` 中的 Lyra 计算。`Task.cancelled()` 可只查询当前请求，`Task.checkCancellation()` 可显式添加检查点。取消跳过普通 `catch`，仍执行已登记的 `defer`。

任意类型的作用域值用 `TaskLocal<T>()` 创建键，通过 `key.get(): Option<T>` 读取，以 `await withLocal(key, value, task)` 激活一个冷作用域。相同类型的两个键互不影响。`taskGroup { ... }` 在当前域运行并等待组内孩子收尾；`parallel { ... }` 在另一执行域启动异步块并返回已启动的 Task。共享读改写可用 [std.sync](../sync/README.md) 显式同步。

`parallelism()` 返回并行 Worker 池容量。进程首次查询或启动并行池时读取 `LYRA_PARALLEL_WORKERS`；未设置时为 `2`，有效范围为 `1`–`64`，无效值会报告 Runtime 配置错误。该值表示并行 Worker 数，不包括调用 `main` 的根执行线程；配置在进程生命周期内固定。

## 通道与等待结果

```lyra
async fn main(): Unit {
    val jobs = Channel<Int>(8)
    val producer = spawn async {
        defer jobs.close()
        await jobs.send(42)
    }
    for await job in jobs { print(job) }
    match awaitOutcome(producer) {
        Completed(_) => {},
        Failed(problem) => throw problem,
        Cancelled(cleanupFailures) => print(cleanupFailures.size),
    }
}
```

`Channel<T>(0)` 直接交接，正容量提供缓冲和背压。关闭后 `for await` 先读完缓冲再结束；退出循环不会自动关闭通道。`await select { val item = jobs.receive() => item, sleepMs(250) => None }` 在收发和计时器间择一提交，同时就绪时按源码顺序。`awaitOutcome` 把目标的成功、失败与取消分别表示为值；等待者自己的取消仍会触发清理。详细契约见[异步 RFC](../../rfc/async-execution.md)。

不用等待时可同步调用 `jobs.trySend(value)` 或 `jobs.tryReceive()`。发送返回 `ChannelSendResult.Sent`、`.Full`（缓冲满或零容量时暂无等待者）、`.Closed`；接收返回 `ChannelReceiveResult.Received(value)`、`.Empty`（暂时没有消息）、`.Closed`（已关闭且缓冲排空）。这两个操作使用同一个队列，也能与已等待的异步收发直接交接。
