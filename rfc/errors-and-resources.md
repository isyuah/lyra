# Lyra RFC：错误与资源清理

**状态：`?` 的错误转换、普通异常的捕获与重抛、同步清理与资源引用语义已接受；具体库 API 仍开放。**  
**范围：** `Result`、`Error`、普通异常、致命故障、取消与作用域清理。

## 两条错误通道与致命故障

预期业务失败通过 `Result<T, E>` 显式返回。程序状态/调用契约遭到破坏时可抛普通异常，由 `try/catch` 处理。取消是独立的协作式控制效果，不是普通 `Error`，也不应被宽泛 catch 意外吞掉。

```text
fn parsePort(text: String): Result<Int, ParseError> { ... }

fn startServer(): Result<Server, Error> {
    val port = parsePort("8080")?
    Server.start(port)
}
```

标准库提供共同的 `Error` 接口。各具体错误可以保留自己的类型；当返回目标使用 `Error` 时，`?` 自动将具体错误包装到共同接口中，并保留原错误及原因链。这样跨多层调用不需每层使用第三方 `anyhow` 容器或手写 `mapErr`。若 API 必须在静态类型上限制错误集合，仍可以使用具体错误类型或错误枚举。

`?` 作用于 `Result` 时，成功取值；失败时可原样传播、提升到标准 `Error`，或在两个具体错误类型之间使用普通 `implicit`。`?` 也可在返回兼容 Optional 的函数中传播 `None`，不隐式互转 `None` 和 `Err`。

普通异常的根类型是 `Exception`。可以按类型捕获并加过滤条件，`catch { ... }` 兜底捕获普通异常，不吞掉取消。catch 中单独写 `throw` 会原样重抛当前异常，保留最初抛出的位置。首版 `throws` 只作文档提示，不改变函数类型，也不强制调用者捕获。`Result::Err` 不会因为携带相同的错误类型就自动抛出。未处理异常到达入口时，运行时输出原因、源位置和异步调用链，并以非零状态退出。稳定错误类型或 kind 是契约，自然语言文案不是。

用 `exception MissingEntry` 声明 `Exception` 的具名子类型，通过 `MissingEntry("missing")` 创建；`pub` 允许导入此类型。这只引入异常类别，不引入普通 Class 继承。`catch reason: MissingEntry if condition { ... }` 先按类型匹配，再用绑定求布尔条件；条件为假继续匹配下一分支。无过滤的根类型捕获或 `catch { ... }` 须放在最后。未匹配的异常原样向外传递。见[异常传递、清理与可运行示例](../compiler/exception-unwinding.md)。

段错误、进程被操作系统终止及运行时损坏不是普通 `catch` 的恢复路径。**暂定的首版 OOM 策略**：只有显式可失败的分配 API 可以用 `Result` 报告本次分配失败；普通对象分配、字符串拼接、集合自动增长等隐式分配耗尽内存时终止。无可恢复 `panic`，需要强制终止时用标准库 `abort`。

语言异常不得未经明确 C ABI 适配越过 C 边界。对外导出函数须在边界转换为错误码、结果结构/回调或明确的致命故障。

<a id="c1-conversions"></a>

## C1：`?` 与普通传参共用 `implicit`

设 `readUser(id)` 返回 `Result<User, DbError>`。在 `ServiceError` 的定义模块声明一次通用转换，传播和普通传参都能用：

```text
pub implicit (error: DbError): ServiceError { ServiceError.database(cause: error) }

fn load(id: Int): Result<User, ServiceError> { Ok(readUser(id)?) }
```

编译器对照内层 `Result` 的错误类型与最近函数或 lambda 的目标错误类型；若是 `async fn`，看其声明的完成值。类型相同直接传播；目标为共同 `Error` 且源错误实现该接口时，使用内置包装。这两步都不用用户转换。其余具体错误类型只查一条有向的 `implicit`，不查专用 trait、标记或同名方法，也不转换整个 `Result`。

**查找规则与 [RFC-0003](0003-types.md) 相同。** 只查源、目标类型的定义模块；声明在本模块可用，跨模块只看 `pub`，不需要单独导入转换。只有 `DbError → StorageError → ServiceError` 两段时，在 `?` 处报缺少直接转换；两端各有一条可见的同向转换时，在 `?` 处报歧义并指向两处声明。不按导入顺序选取。

`load` 遇到 `Ok(user)` 时不执行转换；遇到 `Err(dbError)` 时执行一次，提前返回 `Err(serviceError)`。传播后仍能检查原始 `DbError`、`cause` 和已有的 `suppressed`；若新错误未携带源错误，传播记录须保留两者的关联。普通异常和取消不走这条转换路径。

只有 `reportService(ServiceError)` 一个候选时，`reportService(dbError)` 也使用同一声明。若同一种错误在注册时应映射为冲突、在读取时应映射为服务故障，就不声明通用转换，在各自调用点用 `mapErr` 或 `match` 显式选择。

## 清理和异常安全

`defer` 执行同步清理，在注册它的词法块退出时按注册的逆序各执行一次。适用路径包括：正常完成、`return`、`?` 提前返回、异常展开和协作式任务取消。首期 `defer` 不可挂起或 `await`；影响业务结果的异步 `flush`、`commit` 或协议确认在正常流程中显式 `await`。

若正在展开的异常或取消遇到清理异常，原异常/取消原因仍是主因，清理异常作为附加原因保留；之后的清理仍执行。若块原本正常返回（包括返回 `Err`），清理异常接管这次返回。格式化诊断必须使主因和附加清理失败都能观察。

### 文件和连接是普通引用对象

GC 管理 `File`、`Connection` 等包装对象的内存，不替调用者决定何时关闭操作系统或数据库句柄。用显式 `close`、`defer` 或库提供的作用域 API 确定释放时机。赋值、传参或返回只复制引用，不复制原生句柄，没有 `Owned<T>` 参数、移动或借用检查。可选的漏关诊断是工具能力，不是合法使用资源的前提。

```text
val file = File.open("config.txt")?
val alias = file
file.close()
alias.readAll()   // 已关闭错误；不会拿旧句柄去读
```

一次成功关闭对所有别名可见；后续读写给出可识别的已关闭错误，再次 `close()` 幂等成功。如果仍有读写占用句柄，本次 `close()` 立即抛出 `Busy` 清理异常，保持打开，待操作结束可重试。如果另一个关闭已经开始且尚未完成，也立即报 `Busy`，只有先开始者释放句柄。仅取消等待或触发 GC 都不能提早释放它。[SQLite 验收](native-toolchain.md)覆盖这条路径。

## 已接受验收行为

- `?` 从 `Result` 提取成功值或传播失败；传播保留原始错误内容。
- 一个返回 `Result<T, Error>` 的多层函数可以直接传播实现 `Error` 的不同具体错误，无需逐层编写错误转换。
- `try/catch` 能处理普通语言异常，但不能将致命运行时故障当作已恢复。
- 未捕获异常即便来自未直接 `await` 的结构化子任务也必须可观察。
- 抛出异常、Result 提前返回、取消或正常退出都不跳过已注册的同步清理。
- 清理中的次生异常不得静默抹除当前主异常或其他清理失败。
- 文件别名在关闭后报告已关闭错误；复制含有 `File` 字段的 Struct 也不会复制原生资源。
- 读写未返回时经别名关闭得到 `Busy`；让读写完成后关闭，再经第二个别名关闭成功，原生句柄只释放一次。

## 仍开放

- `Error` 接口的具体继承、错误原因链 API、类型检查与展示接口。
- `throws` 文档标记及其展示方式。
- 可失败分配 API 的最小范围，以及多个清理失败的完整诊断数据结构。
