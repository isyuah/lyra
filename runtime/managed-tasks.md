# 原生 GC 与 Task 接口

本页面向编译器后端和原生库作者，描述 [managed.h](managed.h)、[tasks.h](tasks.h) 的实验性 C ABI。实现为 C++23；可直接用 C 链接器链接。这里的 C++ 测试运行手写的任务状态机，不把它们算作 Lyra 源码端对 `class`、闭包、`async` 或 `blocking` 的验收。

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
| 完成 | `lyraTaskComplete(self, value)`、`lyraTaskFail(self, error)` 保存终态/载荷，唤醒等待者，释放任务帧与运行时强根。`Unit` 可用空指针；其他载荷应存在可追踪的受管理存储中，不能传短命的栈地址。句柄存活时，多次等到同一终态；需要按值复制的结果由调用端读取。 |
| 取消 | `lyraTaskRequestCancel(task)` 设置请求，唤醒可取消的挂起点。回调在恢复入口用 `lyraTaskCancelRequested` 检查；先执行作用域清理，再 `lyraTaskFinishCancelled`。取消一个等待者会撤销它自己的订阅，不替它取消别人已启动的任务。 |

`lyraTaskBlock(self, work, nativeData)` 将同步 C 回调交给 libuv 进程级有界线程池，当前任务挂起，同域仍能执行其他步骤。存有 GC 引用的工作载荷须用 `lyraGcAlloc` 分配；运行时在操作期间保活载荷并注册 Worker，允许原生封装分配对象、复制结果（如 [SQLite 文本](sqlite.md)）。普通 `malloc` 载荷内部仍不扫描。工作线程不调用 Task API；不要同时无同步地修改同一个 C/C++ 字段。被接纳的工作运行中收到取消时，仍等 C 返回才在所属域恢复清理；提交前已经取消则不发起工作。可以用 `UV_THREADPOOL_SIZE=1` 重跑测试，验证不需要额外 Worker 解围。

**当前边界：**多个串行域由同一主线程驱动，可验证跨域等待与恢复归属；显式 CPU 并行域、跨线程 Task API、结构化任务组与上下文/deadline、编译器生成的异常/异步栈与取消清理还需各自的端到端验收。

## 独立验证（Linux）

开发依赖为 Ubuntu `libgc-dev`、`libuv1-dev`、Clang 18。只需 GC 的目标不引入 libuv。三个运行时源分别生成归档成员；纯整数程序不引用 `managed`/`tasks` 符号，即使链接命令列出带 `--as-needed` 的两库，ELF 也无需它们。用 GC/Task 的程序可显式动态链接两库，或在目标构建环境中静态打入相应归档再检查 `DT_NEEDED`。

```sh
out=$(mktemp -d)
clang-18 -std=c++23 -O2 -I runtime runtime/managed.cpp tests/runtime_managed.cpp $(pkg-config --cflags --libs bdw-gc) -o "$out/managed"
clang-18 -std=c++23 -O2 -I runtime runtime/managed.cpp runtime/tasks.cpp tests/runtime_tasks.cpp $(pkg-config --cflags --libs bdw-gc libuv) -o "$out/tasks"
"$out/managed" && "$out/tasks" && UV_THREADPOOL_SIZE=1 "$out/tasks"
readelf -d "$out/tasks" | grep NEEDED
```

[GC 用例](../tests/runtime_managed.cpp)覆盖优化后的局部、闭包捕获、集合中的引用及身份。[Task 用例](../tests/runtime_tasks.cpp)包含只由挂起帧保活的对象、冷启动、多个/晚到的等待者、跨域恢复、异常载荷、取消，以及用条件变量停在 C 调用中的资源；成功路径不靠睡眠猜调度顺序。
