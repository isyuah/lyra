# 原生 GC 与 Task 接口

本页面向编译器后端和原生库作者，描述 [managed.h](managed.h)、[tasks.h](tasks.h) 的实验性 C ABI，以及 LLVM 后端用它生成的 Task 状态机。C++ 测试直接驱动 Runtime；下文列出的源码测试另从 `.ly` 文件构建并运行可执行程序。

## M2：谁保活对象？

- `lyraGcAlloc(bytes)` 返回清零、按机器字扫描引用的内存。字段含受管理引用的对象、闭包捕获区、数组和异步帧使用它。`lyraGcAllocAtomic(bytes)` 用于纯字节数据，不扫描引用，也不保证清零。零字节对象仍有独立身份。普通分配失败是致命错误。
- 第一次在主线程使用时初始化 BDWGC，不需修改整数程序的 `main`。受管理的阻塞池入口会在工作期间注册当前线程栈，结束时只撤销本次建立的注册。任意外部 C 线程反向调用 Lyra 不是已开放的语言能力。对象地址目前不移动，这不是语言的永久地址保证。
- 普通 C `malloc` 区域不是 GC 根。要在原生容器内长期持有对象，用 `lyraGcRootCreate(value)` 创建被扫描的强根；`lyraGcRootGet(root)` 取回对象，`lyraGcRootDrop(root)` 解除保活。丢弃根不表示立即回收，更不负责关闭外部资源。

## M5：生成的状态机怎样挂起和恢复？

| 操作 | ABI 约定 |
|---|---|
| 创建与启动 | `lyraTaskCreate(step, frame)` 只记录入口和已求值的帧，不执行用户代码。首次 `lyraTaskStart(domain, task)` 入队并建强根，至终态才撤销。后来再次启动不重跑、不改域。 |
| 推进域 | `lyraDomainRun(domain)` 在该域驱动 libuv，串行调用就绪的 `step(task, frame)`，返回本次执行的步数。没有就绪任务/活动 libuv 工作时返回；手动门闩未放行的任务可以仍挂起。只在回调外泵循环，域中没有存活任务时再 `lyraDomainDestroy`。 |
| 等待 | 正在运行的 `step` 调用 `lyraTaskAwait(self, target)`。冷目标在等待者域启动；活跃目标保持原域。返回 `0` 时先前保存好恢复点的回调须立即返回；被唤醒时重新检查取消/结果。返回 `1` 时按 `lyraTaskState(target)` 分别处理完成、失败、取消，再用 `lyraTaskOutcome` 读载荷。等待者总是入**自身**域的队列。 |
| 手动门闩 | 设置恢复点后调用 `lyraTaskSuspend(self)` 并返回；域线程可用 `lyraTaskWake(task)` 解除本次门闩。真实外部事件应在清理时撤销旧回调，不把上一轮的通知复用到下一次挂起。 |
| 完成 | `lyraTaskComplete(self, value)`、`lyraTaskFail(self, error)` 等已登记子任务完成后，逆序执行剩余清理，再发布终态并唤醒等待者。`Unit` 可用空指针；其他载荷应存在可追踪的受管理存储中，不能传短命的栈地址。句柄存活时，多次等到同一终态；需要按值复制的结果由调用端读取。 |
| 取消 | `lyraTaskRequestCancel(task)` 设置请求，唤醒可取消的挂起点。回调在恢复入口用 `lyraTaskCancelRequested` 检查；`lyraTaskFinishCancelled` 与其余退出路径共用任务清理。取消一个等待者会撤销它自己的订阅，不替它取消别人已启动的任务。 |

### Lyra 源码如何接入

`async fn work(value: Int): Int` 的函数体返回 `Int`，调用表达式 `work(4)` 则返回 `Task<Int>`。调用时求值实参并保存在受 GC 扫描的堆帧中，不运行函数体。`await` 和 `spawn` 只能写在 `async fn` 中；`spawn` 在当前域启动并返回原句柄，`await` 观察此次任务的结果。不写 `await` 的 `spawn` 仍由异步入口的根域等到结束；只创建未启动的 Task 则不运行。

LLVM 后端给每个异步函数生成一个用于创建冷 Task 的普通函数，以及一个根据恢复点分派的 `step` 回调。局部变量、赋值目标、已求值的实参和跨基本块使用的临时值保存在堆帧上。等待就绪时直接读取结果，否则保存恢复点并返回调度循环；恢复后重新观察终态，不需要嵌套执行事件循环。一次完成值装入受管理的结果盒：重复 `await` 得到独立的 Struct 值或同一 Class 引用。

`async fn main(): Unit` 和 `async fn main(): Result<Unit,String>` 都由 `lyraTaskRunRoot` 建立串行域并启动。首次 `await`/`spawn` 激活的冷任务登记到当前任务；已启动的目标不改归属。父任务在自己的子任务全部收尾后才进入终态。根入口返回 `Ok({})` 时退出码为 0，`Err(message)` 时先等子任务收尾，再向标准错误输出完整 UTF-8 消息（含内嵌 NUL）和换行，进程非零退出。业务 `Err` 是成功交付的值，不触发组取消。

[源码用例](../tests/async_cli.py)在 Debug 和 `--release` 下验证冷实参求值、未启动 Task、丢弃句柄后的收尾、跨挂起 GC 与赋值顺序、重复读取结果、5,000 层递归等待及上述两种入口。[泛型 Task 用例](../tests/generics_tasks_cli.py)验证跨模块的 `Task<Task<T>>` 与类型诊断。

### 词法清理与显式取消

`defer expr` 或 `defer { ... }` 在执行到该语句时登记回调，退出词法块时后进先出。回调捕获绑定，`defer print(number)` 打印清理时的 `number`；块的返回值则在执行清理前求值。正常块尾、`return`、`?`、`break` 和 `continue` 均按退出范围清理。`defer` 内不能 `await`、`spawn` 或请求取消；同步的阻塞 Worker 也可以在自己的线程上登记清理。

异步函数退出时尚未运行的清理保存在堆帧，等本任务所有子任务（含不可中断的 C 工作）安全结束后执行，再把终态交给等待者。`task.cancel()` 只能直接写在 `async fn` 中；重复请求不重复清理，不会连带取消兄弟。对冷 Task 请求取消后再启动，不会进入函数体，也就不会运行体内未登记的清理。语言层用[源码用例](../tests/defer_cli.py)验证逆序、绑定更新、跨挂起 GC、工作线程中 SQLite 句柄释放与重复等待。

### 在 Worker 执行同步代码

`blocking { ... }` 创建冷 `Task<T>`，本身不阻塞，`await` 或 `spawn` 首次启动后将块提交给 libuv 阻塞池。闭包捕获绑定而非当前值：同一个外层 `var` 经同步函数返回、GC 和挂起仍可读写，循环中的新绑定则每轮独立。同步块有自己的返回目标，`return` 和 `?` 均只退出块；没有上下文可推断 `?` 的目标时可给绑定写 `Task<T?>` 或 `Task<Result<T,E>>` 注解。块内不可直接 `await`/`spawn`。对于跨 Worker 共享的捕获与对象字段，编译器对一次读或一次写同步复制；`number += 1` 的读、改、写仍不是一个原子操作。

```text
import std.sqlite.open
async fn main(): Unit {
    val answer = await blocking {
        val db = open(":memory:")
        val query = db.prepare("SELECT 42")
        query.next()
        val number = query.readInt(0).value
        query.close()
        db.close()
        number
    }
    print(answer)
}
```

源码验收还用同一域的写任务放行 FIFO 中阻塞的读任务。在 `UV_THREADPOOL_SIZE=1` 时也能推进；另从真实 SQLite 查询读取文本，确定性关闭后触发 GC，最后在原域读取结果。

### 原生阻塞工作

`lyraTaskBlock(self, work, nativeData)` 将同步 C 回调交给 libuv 进程级有界线程池，当前任务挂起，同域仍能执行其他步骤。存有 GC 引用的工作载荷须用 `lyraGcAlloc` 分配；运行时在操作期间保活载荷并注册 Worker，允许原生封装分配对象、复制结果（如 [SQLite 文本](sqlite.md)）。普通 `malloc` 载荷内部仍不扫描。工作线程不调用 Task API；不要同时无同步地修改同一个 C/C++ 字段。被接纳的工作运行中收到取消时，仍等 C 返回才在所属域恢复清理；提交前已经取消则不发起工作。可以用 `UV_THREADPOOL_SIZE=1` 重跑测试，验证不需要额外 Worker 解围。

`lyraTaskWatchDisconnect(fd)` 从当前域内的任务注册连接断开通知，不读取或关闭调用者的 fd；Linux 后端使用自持的重复 fd。断连时在所属域请求取消当前任务及其激活的孩子；只有 C 工作已返回后才允许父任务执行取消清理。未捕获的原生任务失败取消仍在运行的兄弟，多个失败用 `lyraTaskFailureCount/At` 按登记顺序遍历。实际 socketpair 断开、取消安全与逆序发生的两个失败由[原生任务用例](../tests/runtime_tasks.cpp)验证。

**当前边界：**多个串行域由同一主线程驱动，原生测试验证跨域等待与恢复归属。显式 CPU 并行域、上下文/deadline、普通异常的抛出、异步原因链与清理异常仍需各自的端到端验收。

## 独立验证（Linux）

以下两个原生测试需要 Ubuntu `libgc-dev`、`libuv1-dev`、Clang 18。运行时各源码分别生成归档成员；纯整数程序不引用 `managed`/`tasks` 符号，即使链接命令列出带 `--as-needed` 的两库，ELF 也无需它们。用 GC/Task 的程序可显式动态链接两库，或在目标构建环境中静态打入相应归档再检查 `DT_NEEDED`。

```sh
out=$(mktemp -d)
clang-18 -std=c++23 -O2 -I runtime runtime/managed.cpp tests/runtime_managed.cpp $(pkg-config --cflags --libs bdw-gc) -o "$out/managed"
clang-18 -std=c++23 -O2 -I runtime runtime/managed.cpp runtime/tasks.cpp tests/runtime_tasks.cpp $(pkg-config --cflags --libs bdw-gc libuv) -o "$out/tasks"
"$out/managed" && "$out/tasks" && UV_THREADPOOL_SIZE=1 "$out/tasks"
readelf -d "$out/tasks" | grep NEEDED
```

[GC 用例](../tests/runtime_managed.cpp)覆盖优化后的局部、闭包捕获、集合中的引用及身份。[Task 用例](../tests/runtime_tasks.cpp)包含只由挂起帧保活的对象、冷启动、多个/晚到的等待者、跨域恢复、异常载荷、取消，以及用条件变量停在 C 调用中的资源；成功路径不靠睡眠猜调度顺序。
