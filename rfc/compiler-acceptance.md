# Lyra 首版编译器与运行时：按可观察结果验收

H 由作者委托设计。顺序是先证明**写错能指出原因、写对能独立运行**，再让 GC、异步和 FFI 分别过自己的关。下面是稳定的验收契约；已验证的范围单独列出。

## 第一个切片：同一个程序，只差一个分号

两份源码分别放在 `pass` 和 `fail` 各自的 `src` 下，独立编译，避免同一模块中出现两个 `main`。范围只含具名函数、`Int`、`Unit`、整数加法、函数调用、块末值与最小输出函数 `print(Int)`。先写预期，再做解析、名称解析和类型检查。`check` 到这里结束；`build` 只把通过检查的程序交给 LLVM 生成目标文件、链接小型输出支持。两条命令共享同一个前端。

| 验收编号 | 输入 | 验收条件 |
|---|---|---|
| CC-001 | [fail/src/main.ly](../examples/compiler-smoke/fail/src/main.ly) | `check` 和 `build` 非零退出；返回类型不匹配码 `LYT0001`，主源码范围指向 `total` 末尾的 `;`，实际 `Unit`、期望 `Int`。文本提示可去掉分号；机器可读诊断给出相同的码和范围。此次构建不产生可执行文件。 |
| CC-002 | [pass/src/main.ly](../examples/compiler-smoke/pass/src/main.ly) | `check` 成功；`build` 产出 Linux 原生文件。在[最低验收镜像](native-toolchain.md)上只复制产物、使用列明的系统库，不安装 Lyra 或 LLVM；运行输出 `3` 并正常退出。Debug 和优化构建结果相同。 |

**首片实测：CC-001、CC-002 已通过。** [命令行验收用例](../tests/compiler_cli.py)在 WSL Ubuntu 24.04 和 Ubuntu 22.04 构建环境均通过。在 22.04 链接的 Debug、Release 产物分别复制进干净的官方 Ubuntu Base 22.04.5 amd64 根文件系统；不安装 Lyra、LLVM 或 Clang，均输出 `3` 和换行、退出码为 `0`，ELF 的 `DT_NEEDED` 仅含 `libc.so.6`。[构建说明](../README.md)提供用两阶段容器复现的命令。

首个同步入口为 `fn main(): Unit`，正常返回时进程退出码为 0。`test` 后续把同一批输入分为应通过、应拒绝、应运行三类；断言错误类别、源范围和值，不比较整段措辞或 LLVM IR 文本。先有可信的 `check/build/test`，再补 `fmt/doc` 与编辑器集成。

## Alpha 的交付门槛与复现

一份可信的 Alpha 交付要过三道门：能用自己的包写程序；写错时能定位到业务源码；把编译好的应用复制到没有 Lyra 的服务器仍能运行。以下用例分别验证这些条件；预期值和失败路径在测试里，不依赖手工看 IR。

| 范围 | 可重复验证的行为 |
|---|---|
| M0–M1：工具链、诊断与核心 | `cmake --preset dev` 使用 Clang 18、C++23 命名模块和 Ninja。[前端与原生用例](../tests/compiler_cli.py)覆盖分号返回 `Unit`、副作用顺序、溢出和失败时不覆盖产物。[语法用例](../tests/syntax_cli.py)在 Windows 和 Linux 验证换行、反引号、泛型尖括号与 `await`/`?`/负号优先级。 |
| M2：数据与 GC | [对象测试](../tests/objects_cli.py)检查值复制和引用身份；[文本测试](../tests/text_cli.py)检查 Unicode、无损字节转换和跨 `await` 的插值。[原生任务测试](../tests/runtime_tasks.cpp)让 GC 在任务挂起期间运行；恢复后能读到同一对象。 |
| M3：类型、集合与调用 | [泛型](../tests/generics_cli.py)、[集合](../tests/collections_cli.py)、[和类型](../tests/sums_cli.py)、[隐式转换](../tests/conversions_cli.py)以及[可空与遍历](../tests/sugar_cli.py)分别核对推断、默认值、展开、空值层数、提前返回和转换歧义。[普通函数重载](../tests/overloads_cli.py)核对逐实参排序、胜出后才报转换歧义、默认值只执行一次、函数值和跨包公开候选。 |
| M3：泛型对象与公开 API | [对象](../tests/objects_cli.py)核对实例/静态方法重载和结构约束分派；[模块](../tests/modules_cli.py)核对跨包泛型枚举、扩展组导入和优先级，以及嵌套签名的私有类型检查。 |
| 日常类型推断 | [返回推断与可空收窄](../tests/inference_cli.py)检查私有函数、泛型方法、递归需标注、短路和提前返回，以及跨 `await` 读取同一局部快照。 |
| M4：模块、包与工具分发 | [安装验收](../tests/installed_cli.py)搬迁完整工具链、发布库、锁定并拉取源码，移走注册中心和生产者源码，在仓库外用离线缓存原生编译并执行。 |
| M5：异步、异常与收尾 | [词法清理](../tests/defer_cli.py)、[异常](../tests/exceptions_cli.py)和[Context](../tests/context_cli.py)区分完成值、普通异常和取消，检查阻塞池、任务树及 `await` 的源位置。 |
| M6：C ABI 与服务 | [HTTP/SQLite](../tests/http_cli.py)使用真实 TCP 和系统 SQLite。另一个连接断开时，数据库工作安全收尾，释放 statement 和连接；服务仍接受后续请求。 |
| M7：宏与独立应用 | [宏](../tests/macros_cli.py)检查显式派生、嵌套字段/数组错误路径、标记、入口装饰器、表达式拼接和受控文件输入。[JSONL CLI](../tests/jsonlines_cli.py)检查正确输入、坏行、流错误以及工具链外的双包部署。 |

运行 `ctest --preset dev --output-on-failure` 执行这些套件；`release` preset 另测优化版编译器。最低部署目标由[Jammy 两阶段验收](../tests/jammy-smoke.sh)分别验证只有 libc 的应用和额外依赖系统 SQLite 的应用；量测条件和结果见[原生工具链](native-toolchain.md)。

**仍属设计、不能当作现有 API：** 用户自定义异常子类及过滤捕获、运行时字段反射和 TOML 派生。编辑器集成、`fmt` 和 `doc` 另行交付。[设计服务](../examples/user-service/README.md)保留最终写法；目前编译运行的是[原生服务](../examples/http-sqlite/README.md)。

## 后续扩展仍要守住的三个验收点

1. **GC 与对象。** 引入 `class` 和现成收集器，先在优化构建中验证局部值、闭包捕获和集合持有的引用在触发 GC 后仍可读。测试存活性与对象身份，不规定地址、精确回收时刻或固定内存数字。
2. **异步与诊断。** 用测试门闩让 Task 挂起，只留下异步帧持有对象；触发 GC 后恢复并读取原值。冷 Task 重复等待只执行一次；跨域等待在等待者域恢复。让子任务抛出未捕获异常，报告要能关联抛出、任务创建、`await` 的源码位置。加入 `async fn main(): Result<Unit, E>`（`E` 为业务错误）入口：由根任务组启动并收尾，`Ok` 携带 `Unit` 表示正常完成，`Err` 打印原因链到标准错误并非零退出，不把业务错误转成异常。
3. **C ABI 与真实服务。** 先通过[同步 SQLite 的句柄与取消用例](native-toolchain.md)，再用[小服务的四条请求路径](../examples/user-service/README.md)验证 200、404、数据库失败和断开连接后的清理。`Debug` 与优化构建都验证原生调用返回前不释放句柄；发布包列出系统 SQLite 动态依赖。最后才在同负载下与 Go 比较多进程总内存、启动时间、吞吐和尾延迟。

这样每一步都有单独的失败信号：例如最小程序过不了诊断测试时，只查前端；同一个对象只在跨 `await` 的优化构建中失效时，查异步帧的 GC 根，不必先调试 HTTP。
