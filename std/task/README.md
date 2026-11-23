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

这里使用现有语法和普通泛型函数。RFC 中的 `with Task.scope(...) { ... }` 和面向 Lyra 的通用类型化 `Task.local<T>` 尚未发布；运行时已能按不透明键继承或遮盖任务局部值。
