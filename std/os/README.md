# std.os：程序参数与环境

- `args(): Result<List<String>, IoError>` 返回独立的参数列表，不含程序名。空参数和参数边界原样保留；不能解码的参数返回 `InvalidUtf8`。
- `env(name): Result<String?, IoError>` 区分未设置 `Ok(None)`、已设置为空 `Ok(Some(""))` 和读取失败。名称不能是空字符串或含 `=`／NUL。错误不包含环境变量的值。
- 使用 `fn main(): Int` 或 `async fn main(): Int` 返回 `0..255`。先执行 `defer`，异步入口还会等待结构化子任务收尾；范围外的结果打印诊断并退出 `1`。
- `platform()` 返回 `Platform.Windows` 或 `Platform.Linux`；编译期常量 `architecture` 当前为 `"x86_64"`。
- `waitSignal(signal)` 是一次性冷 Task。Linux 支持 `Interrupt`（SIGINT）、`Terminate`（SIGTERM）；Windows 控制台支持 `Interrupt`（Ctrl+C）、`Break`（Ctrl+Break）。其他组合返回 `Err(Unsupported)`。任务激活后才订阅；收到信号或取消时先移除监听再完成任务。Windows 强制结束进程不会发送可捕获的信号。

`await runProcess(["git", "status"], ProcessOptions(timeoutMs: 5000))` 把程序和参数分别交给 libuv，不把参数拼成 shell 命令；程序查找和可执行文件格式遵循宿主规则。子进程继承环境，默认 stdin 关闭。指定 `ProcessOptions(stdin: bytes)` 后，输入与 stdout / stderr 同时传输，写完自动发送 EOF；子进程提前关闭 stdin 不会替代它本身的退出状态。stdout 和 stderr 分别以 `Bytes` 返回，合计最多 `maxOutputBytes`。非零退出码和信号属于 `Ok(ProcessResult)`，启动、管道、超时和超限属于 `Err(ProcessError)`。

`runProcess` 返回冷任务，激活时获取参数快照。默认超时 30 秒、输出预算 1 MiB；允许 1 毫秒至 7 天和 1 字节至 64 MiB，可设置 `cwd`。超时包含等待阻塞池的时间；超时、取消和超限会关闭捕获管道，终止并回收已经启动的直接子进程。不管理子进程自己创建的后代。主动取消使用 Task 的取消终态，由 `awaitOutcome` 观察。
