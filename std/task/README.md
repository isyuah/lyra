# std.task：不用逐层传 context

```text
import std.task.trace
import std.task.withTrace
import std.task.withTimeout

async fn query(): String { await blocking { trace() } }
async fn main(): Unit {
    val cold = query()
    val label = await withTrace("request-7", withTimeout(5000, cold))
    print(label)
}
```

`withTrace(label, task)` 和 `withTimeout(milliseconds, task)` 返回冷 Task。第一次 `await` 或 `spawn` 才创建局部上下文，并在该作用域内激活传入的任务；离开后调用方的上下文保持原样。`trace()` 返回当前追踪标识（根为 `""`）。阻塞池中的同步代码也能读取属于本次任务的快照，线程复用不混用标识。

`remainingMs()` 返回当前有效截止时间的剩余毫秒；没有时为 `-1`，已到期为 `0`。时间用单调时钟计算，子作用域只可缩短期限，非正超时立即请求取消。到期取消面对的是当前任务树；如果传入的是**已经由另一个作用域启动**的任务，只终止这里的等待。取消不强停同步 C 调用，已启动的工作必须安全收尾。

任意类型的作用域值用 `TaskLocal<T>()` 创建键，通过 `key.get(): Option<T>` 读取，以 `await withLocal(key, value, task)` 激活一个冷作用域。相同类型的两个键互不影响。`taskGroup { ... }` 在当前域运行并等待组内孩子收尾；`parallel { ... }` 在另一执行域启动异步块并返回已启动的 Task。共享读改写可用 [std.sync](../sync/README.md) 显式同步。
