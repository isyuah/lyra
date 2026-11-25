# Lyra 首版编译器与运行时：按可观察结果验收

H 由作者委托设计。顺序是先证明**写错能指出原因、写对能独立运行**，再让 GC、异步和 FFI 分别过自己的关。下面是稳定的验收契约；已验证的范围单独列出。

## 第一个切片：同一个程序，只差一个分号

两份源码分别放在 `pass` 和 `fail` 各自的 `src` 下，独立编译，避免同一模块中出现两个 `main`。范围只含具名函数、`Int`、`Unit`、整数加法、函数调用、块末值与最小输出函数 `print(Int)`。先写预期，再做解析、名称解析和类型检查。`check` 到这里结束；`build` 只把通过检查的程序交给 LLVM 生成目标文件、链接小型输出支持。两条命令共享同一个前端。

| 验收编号 | 输入 | 验收条件 |
|---|---|---|
| CC-001 | [fail/src/main.ly](../examples/compiler-smoke/fail/src/main.ly) | `check` 和 `build` 非零退出；返回类型不匹配码 `LYT0001`，主源码范围指向 `total` 末尾的 `;`，实际 `Unit`、期望 `Int`。文本提示可去掉分号；机器可读诊断给出相同的码和范围。此次构建不产生可执行文件。 |
| CC-002 | [pass/src/main.ly](../examples/compiler-smoke/pass/src/main.ly) | `check` 成功；`build` 产出本机 PE 或 ELF。在[部署目标](native-toolchain.md)上只复制产物、使用列明的系统库，不安装 Lyra 或 LLVM；运行输出 `3` 并正常退出。Debug 和优化构建结果相同。 |

**首片实测：CC-001、CC-002 已通过。** [命令行验收用例](../tests/compiler_cli.py)在 WSL Ubuntu 24.04 和 Ubuntu 22.04 构建环境均通过。在 22.04 链接的 Debug、Release 产物分别复制进干净的官方 Ubuntu Base 22.04.5 amd64 根文件系统；不安装 Lyra、LLVM 或 Clang，均输出 `3` 和换行、退出码为 `0`，ELF 的 `DT_NEEDED` 仅含 `libc.so.6`。[构建说明](../README.md)提供用两阶段容器复现的命令。

首个同步入口为 `fn main(): Unit`，正常返回时进程退出码为 0。`test` 后续把同一批输入分为应通过、应拒绝、应运行三类；断言错误类别、源范围和值，不比较整段措辞或 LLVM IR 文本。先有可信的 `check/build/test`，再补 `fmt/doc` 与编辑器集成。

## Alpha 的交付门槛与复现

一份可信的 Alpha 交付要过三道门：能用自己的包写程序；写错时能定位到业务源码；把编译好的应用复制到没有 Lyra 的服务器仍能运行。以下用例分别验证这些条件；预期值和失败路径在测试里，不依赖手工看 IR。

| 范围 | 可重复验证的行为 |
|---|---|
| M0–M1：工具链、诊断与核心 | `cmake --preset dev` 使用 Clang 18、C++23 命名模块和 Ninja。[前端与原生用例](../tests/compiler_cli.py)覆盖分号返回 `Unit`、副作用顺序、溢出和失败时不覆盖产物。[语法用例](../tests/syntax_cli.py)在 Windows 和 Linux 验证换行、反引号、泛型尖括号与 `await`/`?`/负号优先级。 |
| M2：数据与 GC | [对象测试](../tests/objects_cli.py)检查值复制和引用身份；[文本测试](../tests/text_cli.py)检查 Unicode、无损字节转换和跨 `await` 的插值。[原生任务测试](../tests/runtime_tasks.cpp)让 GC 在任务挂起期间运行；恢复后能读到同一对象。 |
| M3：类型、集合与调用 | [泛型](../tests/generics_cli.py)、[集合](../tests/collections_cli.py)、[和类型](../tests/sums_cli.py)、[隐式转换](../tests/conversions_cli.py)以及[可空与遍历](../tests/sugar_cli.py)分别核对推断、默认值、展开、空值层数、OR / 整数范围穷尽匹配、提前返回和转换歧义。[普通函数重载](../tests/overloads_cli.py)核对逐实参排序、胜出后才报转换歧义、默认值只执行一次、函数值和跨包公开候选。 |
| M3：泛型对象与公开 API | [对象](../tests/objects_cli.py)核对隐式 `this`、实例/静态方法重载与只读扩展属性；[泛型](../tests/generics_cli.py)检查关联类型、多重结构约束与静态分派；[模块](../tests/modules_cli.py)核对跨包泛型枚举、扩展组导入和优先级，以及嵌套签名的私有类型检查。 |
| 日常类型推断 | [返回推断与可空收窄](../tests/inference_cli.py)检查私有函数、泛型方法、递归需标注、短路和提前返回，以及跨 `await` 读取同一局部快照。 |
| M4：模块、包与工具分发 | [安装验收](../tests/installed_cli.py)搬迁 SDK 后锁定并拉取源码，移走注册中心和生产者源码，在无宿主编译工具搜索环境下用离线缓存和预编译 C 静态库构建、执行。还从同一 SDK 构建和运行两个双包应用。 |
| M5：异步、异常与收尾 | [词法清理](../tests/defer_cli.py)、[异常](../tests/exceptions_cli.py)和[Context](../tests/context_cli.py)区分业务错误、普通异常和取消。[异步源码用例](../tests/async_cli.py)核对组的清理与错误顺序、类型化任务局部值和有界 `parallelFor`；[原生任务用例](../tests/runtime_tasks.cpp)用门闩验证不同 OS 线程推进、work stealing、单 Worker、等待者回到自身域、跨线程 GC 和取消时等待 C 工作收尾。 |
| M6：C ABI 与服务 | [HTTP/SQLite](../tests/http_cli.py)使用真实 TCP 和 SQLite。另一个连接断开时，数据库工作安全收尾，释放 statement 和连接；服务仍接受后续请求。 |
| 编译期计算 | [常量用例](../tests/const_cli.py)核对普通纯同步函数、临时 Class/List、闭包与共享捕获、跨包常量身份和重导出；检查纯度、范围、依赖环、预算和拒绝将可变引用保存为常量。 |
| M7：宏与独立应用 | [宏](../tests/macros_cli.py)检查带范围的语法节点、函数/类装饰器、显式派生、生成公开声明的授权和受控输入缓存；参数化查询的快照过期、缺失和类型错误各自报到源码。[JSONL CLI](../tests/jsonlines_cli.py)检查正确输入、坏行、流错误和离线双包部署。 |
| M8：日常标准库与 HTTP | [文件与集合](../tests/stdlib_daily_cli.py)核对字节 I/O、EOF/失败和集合链式调用；[JSON codec](../tests/json_codec_cli.py)与[TOML](../tests/toml_cli.py)核对跨包编解码、未知键与路径、默认值和不可无损表示的状态；[HTTP/SQLite](../tests/http_cli.py)覆盖 Router、手写 handler、`#post`、重复 query、JSON body 和固定并发容量。 |
| M9：LSP 与编辑器 | [协议测试](../tests/lsp_cli.py)检查未保存文件、依赖变化、UTF-16、宏源码映射、补全、悬停、定义、引用、重命名和格式化；[VS Code 联调](../editors/vscode/README.md#真实编辑器验收)用双包项目验证跨包跳转、磁盘变化和带中文与 emoji 的未保存诊断。 |
| 原生调试和反射 | [调试用例](../tests/debug_info_cli.py)检查 DWARF、CodeView/PDB、复合布局、优化版符号与原生回溯。[反射用例](../tests/reflection_cli.py)检查规范类型名、只读公开字段和字段值的类型身份。 |

LSP 按入口包和可达依赖复用分析快照。影响该闭包的源码、清单、锁文件、标准库或授权宏输入变化会重新链接并检查闭包，复用未变且未展开宏的文件的 AST；不相关的包不触发重检。不涉及宏且 token、源码范围都不变的编辑可复用已有类型检查结果。标准库也使用未保存文档的内容。没有函数级增量类型检查。原生目标文件缓存按精确 IR、LLVM 后端版本和发射选项匹配，并校验缓存内容；读写失败或缓存损坏时重新编译。该缓存不跳过前端或链接。

**SDK 验收。** [SDK 安装验收](native-toolchain.md#打包与复验-sdk)在 Windows 和 Ubuntu 22.04 测试搬迁、依赖锁定、预编译 C 静态库和两个双包应用。Ubuntu 另在裸系统分别验证 SDK 编译和独立应用执行。Windows 清空构建工具环境后编译、执行并检查 PE 系统导入项；全新 Windows 虚拟机首跑仍作为发布复验。编辑器可用同两个项目按 [VS Code 联调步骤](../editors/vscode/README.md#真实编辑器验收)验证。

`lyra fmt` 与 LSP 共用[词法格式化用例](../tests/format_cli.py)：整理缩进和行尾空白，保留语义换行、注释和字面量。`std.reflect` 只读公开字段，完整版反射和文档生成后置。[设计服务](../examples/user-service/README.md)保留早期目标写法；Alpha 编译运行的是[原生服务](../examples/http-sqlite/README.md)。

## 后续扩展仍要守住的三个验收点

1. **GC 与对象。** 引入 `class` 和现成收集器，先在优化构建中验证局部值、闭包捕获和集合持有的引用在触发 GC 后仍可读。测试存活性与对象身份，不规定地址、精确回收时刻或固定内存数字。
2. **异步与诊断。** 用测试门闩让 Task 挂起，只留下异步帧持有对象；触发 GC 后恢复并读取原值。冷 Task 重复等待只执行一次；跨域等待在等待者域恢复。让子任务抛出未捕获异常，报告要能关联抛出、任务创建、`await` 的源码位置。加入 `async fn main(): Result<Unit, E>`（`E` 为业务错误）入口：由根任务组启动并收尾，`Ok` 携带 `Unit` 表示正常完成，`Err` 打印原因链到标准错误并非零退出，不把业务错误转成异常。
3. **C ABI 与真实服务。** 先通过[同步 SQLite 的句柄与取消用例](native-toolchain.md)，再用[小服务的四条请求路径](../examples/user-service/README.md)验证 200、404、数据库失败和断开连接后的清理。`Debug` 与优化构建都验证原生调用返回前不释放句柄；部署验收检查 SQLite 无额外动态库依赖。最后才在同负载下与 Go 比较多进程总内存、启动时间、吞吐和尾延迟。

这样每一步都有单独的失败信号：例如最小程序过不了诊断测试时，只查前端；同一个对象只在跨 `await` 的优化构建中失效时，查异步帧的 GC 根，不必先调试 HTTP。
