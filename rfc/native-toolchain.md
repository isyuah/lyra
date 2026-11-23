# Lyra RFC：原生工具链、GC 与 C ABI

**状态：LLVM AOT、Linux x86-64/glibc 首个验收目标、系统 SQLite 动态库及 GC 演进路线已定；收集器、布局和底层 ABI 待原型验证。**  
**范围：** 原生编译与运行、发布目标、现成 GC 方向、SQLite C ABI 首个用例及错误诊断目标。

## Native 编译和发布

- 编译模型为 LLVM 后端的 AOT 原生编译。
- 用户程序不得依赖 JVM、CLR 或 Lyra 托管虚拟机；程序可以携带语言 Runtime。应用与 Runtime 的代码被本机 linker 链接。
- 首期工作流以 Windows 开发机通过 WSL/Linux 环境构建并部署到 Linux 服务器为目标。允许依赖系统动态库；Lyra、LLVM 和语言 VM 不需要安装在目标机。
- 首个验收目标是 Linux x86-64/glibc，以 Ubuntu 22.04 x86-64 为最低测试镜像，用相应目标库链接。二进制的架构、libc 和动态库依赖须可检查。[整数切片已实测](compiler-acceptance.md)，仅动态依赖系统 libc；后续 SQLite 切片还须使用并列出系统 SQLite 动态库。
- 类 Go `scratch` 的零基础镜像/全静态交付是后续优化目标，不是首期门槛。不得以支持 scratch 为由让第一版构建链无法运行。

程序包大小与常驻内存分别看待。目标是内置 Runtime/GC 但不用虚拟机、单程序易分发且能够在服务器上多进程运行；目前不设与 Go 对比的硬 MB 上限。建立可执行服务后，再以等价负载与 Go 对照报告多进程总内存、二进制大小、启动时间、吞吐和尾延迟。

### 首个编译器的实现选择

编译器使用 C++20、CMake 和 CTest；最小运行时用 C11。`check` 和 `build` 共用词法、语法、名称与类型检查，前端不调用 Clang。后端用 LLVM 18 的 C++ `IRBuilder` 构造模块，验证后写临时 IR，由 `clang-18` 生成目标文件并链接 C 运行时。以后若改为进程内生成目标文件，替换模块验证之后的步骤即可。发布的是 Lyra 原生程序，不包含编译器或 LLVM；这个整数切片只动态依赖系统 libc。

### 开发机准备：WSL 2 + Ubuntu 24.04

写规范、设计一致性用例不需要安装 LLVM。要构建当前编译器，只在 WSL/Linux 安装 LLVM 18.1 开发包和 Clang 18 即可，不用在 Windows 再配 SDK，也不用从源码编译 LLVM。正式验收另用 22.04 目标环境或对应 sysroot 构建，并在最低镜像上执行，不能拿 24.04 本机链接、运行成功代替。

1. 在 PowerShell 运行 `wsl -l -v`。已有 `Ubuntu-24.04`（版本 2）就直接 `wsl -d Ubuntu-24.04`；新开发机没有该发行版时才运行 `wsl --install -d Ubuntu-24.04`，按系统提示完成首次启动。
2. 进入 **Ubuntu 终端**后，先运行 `clang-18 --version`、`llvm-config-18 --version --host-target --cmakedir` 和 `cmake --version`。头文件、库和 `LLVMConfig.cmake` 来自开发包，不能只验证 `clang`。缺包时才安装：

   ```sh
   sudo apt update
   sudo apt install build-essential clang-18 llvm-18 llvm-18-dev cmake
   ```

3. 在同一 Ubuntu 终端做一次冒烟检查。`LLVMConfig.cmake` 应可读；IR 编译链接出的程序应返回 `42`：

   ```sh
   (
     set -eu
     test -r "$(llvm-config-18 --cmakedir)/LLVMConfig.cmake"
     out=$(mktemp)
     trap 'rm -f -- "$out"' EXIT
     printf '%s\n' 'define i32 @main() { ret i32 42 }' | clang-18 -Wno-override-module -O2 -x ir - -o "$out"
     status=0; "$out" || status=$?
     printf 'exit=%s (expected 42)\n' "$status"
     test "$status" -eq 42
   )
   ```

用同一套 `llvm-config-18` 查开发库路径；构建命令和 CTest 用法见[仓库说明](../README.md)。Make 已足够，选用 Ninja 生成器时才需 `ninja-build`。GC 和 SQLite 开发包留到各自原型，不是这一步的前置条件。

## 编译器、Runtime 和 GC

LLVM 是代码生成后端，不是语言运行时或 GC。Lyra Runtime 实现任务调度、异常/诊断支持、对象布局与外部运行时协作；GC 算法优先选用维护成熟的现成实现，不自建第一版收集器。

路线分两步：先用成熟的非移动保守 GC 做原型评估（例如 BDWGC），长期演进到精确根追踪。具体收集器按优化构建、异步帧和原生线程注册的实测选择。无论哪一步，异步 Task Frame、闭包、全局状态和已注册的 C 句柄中的存活引用都不能丢失；普通 C `malloc` 区域不得成为唯一活引用的无追踪存储位置。

语言不保证对象物理地址稳定，也不依赖终结器及时关闭外部资源。精确根追踪是后端目标，不等于必须采用移动式 GC。Safepoint、对象 ABI、FFI pin/handle 及 LLVM lowering 方案由后续原型确定。编译器和 Runtime 还须满足[跨域竞争时的内存安全底线](async-execution.md)；不能直接把语言允许的竞争降低为 LLVM 中可导致任意行为的普通并发读写。

## 首个 C ABI 用例：SQLite

第一版通过显式 `extern` 声明绑定 C 标量、指针和不透明句柄。字符串按每个 C 接口的契约明确传长度或使用终止符；SQLite 文本在下一次 step 或 finalize 使原缓冲失效前复制为 Lyra 字符串。首个绑定是按需导入的官方 SQLite 包：打开连接、准备/执行语句、绑定值、迭代结果、确定性关闭 statement 和 connection。C 保存 Lyra 闭包、外部线程反向回调和任意 C 头文件自动导入不纳入首版。

在异步任务中，可能阻塞的 SQLite 操作必须通过隔离的执行入口进行。首例可以在一次隔离的同步操作里打开数据库、执行查询并清理，再把数据值返回执行域。这样 `defer` 仍是同步的，也不会在调度线程上执行可能阻塞的关闭操作。

**端到端验收（待实现）**：用真实 SQLite 内存库建表，插入 `id=7, name="Ada"`，prepare 带参数的查询，绑定 `7` 并 step，拷出 `"Ada"` 后 finalize statement，再关闭 connection。SQLite 调用失败在 Lyra 封装边界映射为可检查的 `Result::Err`，不让 C 错误码或语言异常穿过 C ABI；例如用不存在的表令 prepare 失败，并确认已有 connection 仍关闭。

取消路径单独验收：用可控的测试门闩停住隔离操作，同域另一 Task 应仍能推进。取消拥有该操作的作用域时，**不能在原生操作还可能使用句柄时释放它**；放行并等它安全返回，清理后任务组才可退出。不要求不支持中断的 C 调用立即停止。这两条用例不用定时睡眠来猜调度顺序。

公开 `Connection` 是[普通资源引用](errors-and-resources.md)，可以传参、返回或存入对象；离开注册了 `defer close` 的块时仍照常关闭，带出引用不会延长原生资源的打开期。使用中或正在关闭的连接遇到另一次 `close()` 要报 `Busy`；关闭成功后再关则幂等。原始 C 指针、固定与释放留在不安全 FFI 边界；安全封装须遵守 SQLite 线程模式及 statement 的生命周期。

## 标准库与诊断

- `async`、`await` 等影响 Task 控制流的能力由语言和 Runtime 配合支持；核心 Runtime 和基础库随工具链，HTTP、JSON/TOML 作为官方维护、按需导入的库包，可独立发修复版。首个编译器切片的正反例、入口与后续验证顺序见[编译器验收设计](compiler-acceptance.md)。
- 未捕获异常诊断应包含源码位置、错误原因及异步调用链；用户应能从 `await` 返回的失败看到任务被启动和挂起的位置。
- 前端的 `check`/类型诊断在 LLVM 代码生成前完成；输出稳定错误码、主源码范围、原因、相关类型或调用点，支持机器可读 JSON。测试断言类别和源码范围，不锁定自然语言文案或 LLVM 内部报错。

## 已接受验收行为

- 指定 Linux 目标生成原生可执行文件，不要求安装 Lyra VM/LLVM Runtime；动态库依赖明确可查。
- WSL/Linux 构建的产物在声明的目标 Linux 环境中执行。
- 同步 SQLite 调用不阻塞串行异步执行域 Worker；SQLite 的外部资源可确定性关闭。
- 任务暂停期间仅由异步状态帧持有的对象仍能通过 GC 压力测试。
- 异步异常报告能沿源调用链回溯到 `await` 和原始任务创建位置。
- 有/无动态 SQLite 依赖的交付行为显式区分；未来支持 scratch 时通过独立目标配置声明。

## 仍开放

- 额外架构、libc/链接器组合及全静态支持矩阵。
- 具体收集器、线程注册 API、精确根追踪与优化级别的交互。
- Runtime 默认管理 GC 和 Worker，并提供基本限制和统计；具体配置、指标和默认值由原型测量。
- `extern` 声明的完整 ABI 类型表、错误码到 Result 的具体映射、内部句柄表示和中断接口。
- 官方库包与工具链的版本兼容范围。
