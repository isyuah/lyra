# 原生 GC 与 Task 接口

本页面向编译器后端和原生库作者，描述 [managed.h](managed.h)、[tasks.h](tasks.h) 的实验性 C ABI，以及 LLVM 后端用它生成的 Task 状态机。C++ 测试直接驱动 Runtime；下文列出的源码测试另从 `.ly` 文件构建并运行可执行程序。

## M2：谁保活对象？

- `lyraGcAlloc(bytes)` 返回清零、按机器字扫描引用的内存。字段含受管理引用的对象、闭包捕获区、数组和异步帧使用它。`lyraGcAllocAtomic(bytes)` 用于纯字节数据，不扫描引用，也不保证清零。零字节对象仍有独立身份。普通分配失败是致命错误。
- 第一次在主线程使用时初始化 BDWGC，不需修改整数程序的 `main`。受管理的阻塞池入口在调用期间注册线程栈；显式并行的执行器从启动到退出保持注册。两者都只撤销本次建立的注册。任意外部 C 线程反向调用 Lyra 不是已开放的语言能力。对象地址目前不移动，这不是语言的永久地址保证。
- 普通 C `malloc` 区域不是 GC 根。要在原生容器内长期持有对象，用 `lyraGcRootCreate(value)` 创建被扫描的强根；`lyraGcRootGet(root)` 取回对象，`lyraGcRootDrop(root)` 解除保活。丢弃根不表示立即回收，更不负责关闭外部资源。

## M5：生成的状态机怎样挂起和恢复？

| 操作 | ABI 约定 |
|---|---|
| 创建与启动 | `lyraTaskCreate(step, frame)` 只记录入口和已求值的帧，不执行用户代码。首次 `lyraTaskStart(domain, task)` 入队并建强根，至终态才撤销。后来再次启动不重跑、不改域。 |
| 推进域 | `lyraDomainRun(domain)` 在该域的执行器上驱动 libuv，串行调用就绪的 `step(task, frame)`，返回本次执行的步数。没有就绪任务、活动 libuv 工作或来自 Worker 域的待交付结果时返回；手动门闩未放行的任务可以仍挂起。只在回调外泵循环，域中没有存活任务时再 `lyraDomainDestroy`。 |
| 显式换域 | `lyraTaskParallel(parent, target)` 接收当前运行任务和一个冷 Task，在不同执行器上的新域登记并启动子任务；若目标已激活则只返回原句柄。`lyraTaskSpawn` 则仍把冷子任务放进父域。两者都从首次激活处继承 Context，子任务的收尾由登记它的父任务负责。 |
| 局部上下文 | `lyraTaskWithLocal(key, boxed, body)` 创建冷包装任务，首次激活时在继承的 Context 中加入该键和值，再等待 `body`。冷 `body` 获得扩展后的 Context；已激活的 `body` 保留原 Context 和归属。键和值盒都由 GC 保活，包装任务转发结果、异常与取消。 |
| 等待 | 正在运行的 `step` 调用 `lyraTaskAwait(self, target)`。冷目标在等待者域启动；活跃目标保持原域。返回 `0` 时保存过恢复点的回调须立即返回；返回 `1` 时按 `lyraTaskState(target)` 分支，成功只读 `lyraTaskOutcome`，失败读 `lyraTaskException`。等待者总是入**自身**域的队列。 |
| 手动门闩 | 设置恢复点后调用 `lyraTaskSuspend(self)` 并返回；域线程可用 `lyraTaskWake(task)` 解除本次门闩。真实外部事件应在清理时撤销旧回调，不把上一轮的通知复用到下一次挂起。 |
| 完成 | `lyraTaskComplete(self, value)`、`lyraTaskFail(self, error)` 等已登记子任务完成后，逆序执行剩余清理，再发布终态并唤醒等待者。`Unit` 可用空指针；其他载荷应存在可追踪的受管理存储中，不能传短命的栈地址。句柄存活时，多次等到同一终态；需要按值复制的结果由调用端读取。 |
| 取消 | `lyraTaskRequestCancel(task)` 设置请求，唤醒可取消的挂起点。回调在恢复入口用 `lyraTaskCancelRequested` 检查；`lyraTaskFinishCancelled` 与其余退出路径共用任务清理。取消一个等待者会撤销它自己的订阅，不替它取消别人已启动的任务。 |

### Lyra 源码如何接入

`async fn work(value: Int): Int` 的函数体返回 `Int`，调用表达式 `work(4)` 则返回 `Task<Int>`。调用时求值实参并保存在受 GC 扫描的堆帧中，不运行函数体。`await` 和 `spawn` 只能写在 `async fn` 中；`spawn` 在当前域启动并返回原句柄，`await` 观察此次任务的结果。不写 `await` 的 `spawn` 仍由异步入口的根域等到结束；只创建未启动的 Task 则不运行。

LLVM 后端给每个异步函数生成一个用于创建冷 Task 的普通函数，以及一个根据恢复点分派的 `step` 回调。局部变量、赋值目标、已求值的实参和跨基本块使用的临时值保存在堆帧上。等待就绪时直接读取结果，否则保存恢复点并返回调度循环；恢复后重新观察终态，不需要嵌套执行事件循环。一次完成值装入受管理的结果盒：重复 `await` 得到独立的 Struct 值或同一 Class 引用。

`async fn main(): Unit` 和返回 `Result<Unit,String>` 或 `Result<Unit,Error>` 的异步入口都由 `lyraTaskRunRoot` 建立串行域并启动。首次 `await`/`spawn` 激活的冷任务登记到当前任务；已启动的目标不改归属。父任务在自己的子任务全部收尾后才进入终态。根入口返回 `Ok({})` 时退出码为 0，`Err` 时先等子任务收尾，再向标准错误输出完整 UTF-8 消息（含内嵌 NUL），共同 `Error` 还输出原因和附加失败。业务 `Err` 非零退出，但不是普通异常，也不触发组取消。

[源码用例](../tests/async_cli.py)在 Debug 和 `--release` 下验证冷实参求值、未启动 Task、丢弃句柄后的收尾、跨挂起 GC 与赋值顺序、重复读取结果、5,000 层递归等待及上述两种入口。[泛型 Task 用例](../tests/generics_tasks_cli.py)验证跨模块的 `Task<Task<T>>` 与类型诊断。

### 词法清理与显式取消

`defer expr` 或 `defer { ... }` 在执行到该语句时登记回调，退出词法块时后进先出。回调捕获绑定，`defer print(number)` 打印清理时的 `number`；块的返回值则在执行清理前求值。正常块尾、`return`、`?`、`break` 和 `continue` 均按退出范围清理。`defer` 内不能 `await`、`spawn` 或请求取消；同步的阻塞 Worker 也可以在自己的线程上登记清理。

异步函数退出时尚未运行的清理保存在堆帧，等本任务所有子任务（含不可中断的 C 工作）安全结束后执行，再把终态交给等待者。`task.cancel()` 只能直接写在 `async fn` 中；重复请求不重复清理，不会连带取消兄弟。对冷 Task 请求取消后再启动，不会进入函数体，也就不会运行体内未登记的清理。语言层用[源码用例](../tests/defer_cli.py)验证逆序、绑定更新、跨挂起 GC、工作线程中 SQLite 句柄释放与重复等待。

### 普通异常如何跨过 `await`？

生成的状态机使用 `lyraTaskAwaitValue`：挂起时返回调度循环，成功时读取结果盒，普通异常时从独立的错误槽进入当前 `catch`。被等待者取消时则直接收尾，不经过普通 `catch`。`lyraTaskFail` 接受 `LyraException`，原生错误需显式适配；不把 C 指针当异常，不让 C++ 异常或 `longjmp` 越过 Worker 回调。Worker 通过异常输出参数交回失败，`defer` 回调直接返回异常或空指针。

每次调用清理前先弹栈。正常退出时第一个清理失败成为主异常；已有异常或取消时保留原主因，后续清理仍运行，所有附加原因按实际执行顺序保留。任务缓存不可变的失败记录，附带抛出及创建位置；每个等待者只给自己的异常视图附加 `await` 位置。未处理的普通异常到达根时打印原因链并非零退出。[源码用例](../tests/exceptions_cli.py)覆盖跨挂起重抛、Worker、`?` 和取消收尾。

[多域原生测试](../tests/runtime_task_errors.cpp)用明确放行的失败任务区分三种情况：直接等待冷任务、在失败发布前等待自己 `spawn` 的孩子、从另一组或发布后才观察它。只有前两种可在本地捕获后让所属组正常结束。单独取消孩子不会取消兄弟；因无人观察的异常而取消的兄弟，其清理失败附在组的原失败之后。

### 在 Worker 执行同步代码

`blocking { ... }` 创建冷 `Task<T>`，本身不阻塞，`await` 或 `spawn` 首次启动后将块提交给 libuv 阻塞池。闭包捕获绑定而非当前值：同一个外层 `var` 经同步函数返回、GC 和挂起仍可读写，循环中的新绑定则每轮独立。同步块有自己的返回目标，`return` 和 `?` 均只退出块；没有上下文可推断 `?` 的目标时可给绑定写 `Task<T?>` 或 `Task<Result<T,E>>` 注解。块内不可直接 `await`/`spawn`。对于跨 Worker 共享的捕获与对象字段，编译器对一次读或一次写同步复制；`number += 1` 的读、改、写仍不是一个原子操作。

```text
import std.sqlite.DbError
import std.sqlite.open
fn readAnswer(): Result<Int?, DbError> {
    val db = open(":memory:")?
    defer db.close()
    val query = db.prepare("SELECT 42")?
    defer query.close()
    if query.next()? == false { return Ok(None) }
    query.readInt(0)
}
async fn main(): Result<Unit, String> {
    val answer = await blocking { readAnswer() }
    match answer {
        Ok(Some(number)) => { print(number); Ok({}) },
        Ok(None) => Err("no answer"),
        Err(error) => Err(error.message()),
    }
}
```

源码验收确认域在 Worker 阻塞读标准输入时仍能推进：测试进程收到同域任务的通知后才写入管道。在 `UV_THREADPOOL_SIZE=1` 时也能推进；另从真实 SQLite 查询读取文本，确定性关闭后触发 GC，最后在原域读取结果。

### 原生阻塞工作

`lyraTaskBlock(self, work, nativeData)` 将同步 C 回调交给 libuv 进程级有界线程池，当前任务挂起，同域仍能执行其他步骤。存有 GC 引用的工作载荷须用 `lyraGcAlloc` 分配；运行时在操作期间保活载荷并注册 Worker，允许原生封装分配对象、复制结果（如 [SQLite 文本](sqlite.md)）。普通 `malloc` 载荷内部仍不扫描。工作线程不调用 Task API；不要同时无同步地修改同一个 C/C++ 字段。被接纳的工作运行中收到取消时，仍等 C 返回才在所属域恢复清理；提交前已经取消则不发起工作。可以用 `UV_THREADPOOL_SIZE=1` 重跑测试，验证不需要额外 Worker 解围。

`lyraTaskWatchDisconnect(monitor)` 从当前任务注册连接断开通知，不读写或关闭请求套接字。Linux 用重复的 fd 加 libuv poll；Windows 给连接登记 Winsock `FD_CLOSE`，用独立的事件句柄和 libuv 通知所属域。断连时取消当前任务及其激活的孩子；C 工作返回后父任务才执行取消清理。未捕获的原生任务失败会取消仍在运行的兄弟，多个失败按登记顺序遍历。[原生任务用例](../tests/runtime_tasks.cpp)验证实际 socket 对断开、取消安全与逆序发生的两个失败。

**任务上下文：**[std.task](../std/task/README.md) 用普通泛型函数提供隐式 trace 和 deadline。冷 Task 创建时没有快照，首次启动时从所有者继承；重复等待不重设。阻塞请求在提交时保留不可变快照，Worker 仅在调用期间可读，不能登记超时或 socket watch。每个更严格的作用域在所属事件循环安装单调时钟定时器；继承同一期限的子任务由上层传播取消，无需另建定时器。只有已经激活的所有者承担收尾责任。[源码用例](../tests/context_cli.py)用单 Worker 和受测试进程控制的标准输入固定等待顺序；[原生用例](../tests/runtime_context.cpp)验证任务局部的 GC 保活、提前结束时撤掉定时器及已挂起任务到期后被唤醒。

### 显式 CPU 并行

原生 `lyraTaskParallel` 懒建一个与 libuv 阻塞池分开的有界池。当前每个手动驱动的根执行器配置两个 Worker；每个 Worker 驱动自己的 libuv 循环，复用给多个逻辑域。挂起的域不独占 Worker，因此嵌套启动和等待不需要再创建线程。分配时避开调用者所在的执行器；只有显式换域会带来同时执行的用户代码，同域的 `spawn` 仍串行。

跨线程发布只同步任务状态、父子关系和就绪队列，再通过 `uv_async_send` 唤醒目标执行器。运行用户步骤和执行 `defer` 时不持调度锁；父任务清理由它自己的执行器执行。根执行器等待已启动的 Worker 子任务完成后再销毁，Worker 退出前清完 libuv 句柄并注销 GC 栈。

[原生门闩用例](../tests/runtime_tasks.cpp)让两个 Worker 和调用者在不同 OS 线程到达同一个门闩，随后跨域 `await` 回到调用者，保留各自的 Context 和只由挂起线程栈持有的对象。用例也覆盖超过池大小的嵌套等待、取消后等不透明 C 调用返回、父域清理和跨域失败排序。`taskGroup` 和 `parallel` 通过[异步源码用例](../tests/async_cli.py)验证块尾值和组收尾、锁的异常安全及跨域上下文。

[原生同步 ABI](sync.md)提供可重入互斥锁与顺序一致的原子整数，用于跨域复合更新。

## 独立验证（Linux）

以下原生测试需要 Ubuntu `libgc-dev`、`libuv1-dev`、Clang 18。它们使用系统提供的 GC/libuv；发布程序则由编译驱动静态打入所需的运行时归档，可用 `readelf -d` 检查目标产物的 `DT_NEEDED`。

```sh
out=$(mktemp -d)
clang++-18 -std=c++23 -O2 -I runtime -I include runtime/managed.cpp runtime/exceptions.cpp runtime/strings.cpp tests/runtime_managed.cpp $(pkg-config --cflags --libs bdw-gc) -o "$out/managed"
clang++-18 -std=c++23 -O2 -I runtime -I include runtime/managed.cpp runtime/tasks.cpp runtime/strings.cpp runtime/context.cpp runtime/exceptions.cpp runtime/net.cpp tests/runtime_tasks.cpp $(pkg-config --cflags --libs bdw-gc libuv) -o "$out/tasks"
"$out/managed" && "$out/tasks" && UV_THREADPOOL_SIZE=1 "$out/tasks"
readelf -d "$out/tasks" | grep NEEDED
```

[GC 用例](../tests/runtime_managed.cpp)覆盖优化后的局部、闭包捕获、集合中的引用及身份。[Task 用例](../tests/runtime_tasks.cpp)包含只由挂起帧保活的对象、冷启动、多个/晚到的等待者、跨域恢复、异常载荷、取消，以及用条件变量停在 C 调用中的资源；成功路径不靠睡眠猜调度顺序。
