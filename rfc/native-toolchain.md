# Lyra RFC：原生工具链、GC 与 C ABI

**状态：LLVM AOT 支持 Windows x64 与 Linux x86-64/glibc 原生构建；原型使用 BDWGC，精确根追踪与内部对象布局仍可演进。**

**范围：** 原生编译与运行、发布目标、现成 GC 方向、SQLite C ABI 首个用例及错误诊断目标。

## Native 编译和发布

- 编译模型为 LLVM 后端的 AOT 原生编译。
- 用户程序不得依赖 JVM、CLR 或 Lyra 托管虚拟机；程序可以携带语言 Runtime。应用与 Runtime 的代码被本机 linker 链接。
- Windows 开发机可直接构建 Windows x64 程序。给 Linux 服务器构建时仍用 Linux 或 WSL；当前不做跨目标链接和 sysroot 管理。
- 首个验收目标是 Linux x86-64/glibc，以 Ubuntu 22.04 x86-64 为最低测试镜像，用相应目标库链接。[部署验收](../tests/jammy-smoke.sh)检查 ELF 的动态依赖：整数、对象、标准 I/O 和 JSONL 样例只需系统 libc；SQLite 与 HTTP/SQLite 样例另需 `libsqlite3.so.0`。
- Windows 目标为 x64 MSVC ABI。GC、libuv、Jansson、llhttp、tomlc17、SQLite 和 C Runtime 静态链接进使用它们的应用。Windows 测试检查 PE 只导入系统 DLL，并从搬迁后的 SDK 离线编译，在无构建工具 `PATH`、`LIB`、`INCLUDE` 的进程中运行独立产物。通过外部导入库引入的 DLL 由应用另行部署。
- 类 Go `scratch` 的零基础镜像/全静态交付是后续优化目标，不是首期门槛。不得以支持 scratch 为由让第一版构建链无法运行。

程序包大小与常驻内存分别看待。目标是内置 Runtime/GC 但不用虚拟机、单程序易分发且能够在服务器上多进程运行；目前不设与 Go 对比的硬 MB 上限。建立可执行服务后，再以等价负载与 Go 对照报告多进程总内存、二进制大小、启动时间、吞吐和尾延迟。

### 首个多进程样本

在 WSL 2 的 Ubuntu 22.04.5 x86-64 独立运行根中测量 `90621d5` 对应的 Release 应用：不安装 Lyra、LLVM、GC 或 libuv，另装系统 `libsqlite3-0`。未 `strip` 的 HTTP/SQLite 文件为 **518,824 字节**，JSONL 为 **254,232 字节**。同时启动四个同一 ELF 的服务进程，使用默认阻塞池；每个在同一只读数据库上成功响应一次 `GET /users/7`，保持监听，再从 `/proc/<pid>/smaps_rollup` 取样。

| 指标 | 四进程合计 | 解释 |
|---|---:|---|
| PSS | 9,770 KiB | 按实际映射数分摊共享页；单进程 2,417–2,479 KiB。 |
| RSS | 21,072 KiB | 每个进程都计入其常驻共享页，直接相加会重复计算。 |

复现：用[部署验收脚本](../tests/jammy-smoke.sh)的 `build` 模式生成应用和数据库，把输出放在干净运行环境的 `apps/`，在该目录的上级执行以下 Bash。`stdbuf` 仅让样例通过管道及时宣布端口，测量和复现都使用它。

```bash
set -eu
stat -c '%n %s bytes' apps/{http,jsonlines}-release
scratch=$(mktemp -d); pids=()
trap 'for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || :; done; wait || :; rmdir "$scratch"' EXIT
for instance in 1 2 3 4; do
    mkfifo "$scratch/port"
    stdbuf -oL apps/http-release apps/http-fixture.db >"$scratch/port" & pids+=("$!")
    read -r port <"$scratch/port"; rm "$scratch/port"
    exec {client}<>"/dev/tcp/127.0.0.1/$port"
    printf 'GET /users/7 HTTP/1.1\r\nHost: localhost\r\n\r\n' >&"$client"
    response=$(cat <&"$client"); exec {client}>&-; [[ $response == *'"Ada"'* ]]
done
for pid in "${pids[@]}"; do cat "/proc/$pid/smaps_rollup"; done |
    awk '/^Pss:/ {pss+=$2} /^Rss:/ {rss+=$2} END {print "PSS",pss,"KiB; RSS",rss,"KiB"}'
```

这是一次极轻负载、暖机后空闲的快照，记录的是应用进程内存，而不是整个 WSL 虚拟机。后续 Go 对照应固定相同的服务、数据库、并发、目标环境和统计方法；这里的数值不作为性能预算。

### 首个编译器的实现选择

编译器和运行时使用 **C++23、Clang、CMake 与 Ninja 1.11+**。源码诊断、语法、类型检查、LLVM lowering 和构建驱动通过 C++ 命名模块暴露接口，CMake `FILE_SET CXX_MODULES` 与宿主 Clang 配套的 `clang-scan-deps` 跟踪依赖，不要求工具链提供 `import std`。`check` 和 `build` 共用前端，前端不调用 Clang。后端用 LLVM 18.1 的 `IRBuilder` 构造模块，验证后用 Clang 将临时 IR 编译为本机目标文件，再链接具有 C ABI 的静态运行时。Linux 使用 `clang-18`；Windows 用 `PATH` 中的 `clang.exe` 和 `lld-link.exe`。

### 调试原生程序

默认 Debug 产物内含 DWARF（Linux）或配套的 CodeView/PDB（Windows）。函数、参数、局部变量、源码行和复合类型字段按 LLVM 目标布局记录；Windows 部署调试产物时将同名 `.pdb` 放在 `.exe` 旁。`lyra build <包目录> --release --debug-info` 可保留优化版符号；优化器删除或合并的局部值仍可能无法查看。

`LYRA_NATIVE_BACKTRACE=1` 在异常首次抛出时捕获至多 32 帧。未捕获异常先打印 Lyra 的源码、任务与原因链，再输出原生帧；原样重抛不会用新的回溯覆盖抛点。Windows 运行时从本机 PDB 查符号，Linux 打印模块名和相对偏移，例如可用 `llvm-symbolizer-18 --obj ./app 0x1234` 查具体位置。合成宏代码的语义诊断仍指回手写调用处。原生调试信息、回溯和可搬迁符号由[命令行用例](../tests/debug_info_cli.py)验证。

### 开发机准备：Windows x64

1. 准备 CMake、Ninja、Python 3（测试）、Git 和 Visual Studio Build Tools 的 C++ 桌面开发组件。打开 **x64 Developer PowerShell**，确保 `LIB` 中有 x64 CRT 和 Windows SDK 的 `um`、`ucrt` 库目录。原生构建已使用 CMake 4.5、Ninja 1.13、Clang 23 和 VS 2026 的 MSVC 工具集；较新的 MSVC STL 需要与它兼容的 Clang，宿主 Clang 不必与嵌入的 LLVM 库同版本。
2. 下载 LLVM 官方 [18.1.8 Windows x64 MSVC 完整归档](https://github.com/llvm/llvm-project/releases/tag/llvmorg-18.1.8)中的 `clang+llvm-18.1.8-x86_64-pc-windows-msvc.tar.xz`，解压到独立目录。`LLVM_DIR` 指向其中的 `lib/cmake/llvm`，仅有 Clang 的安装包不含编译 Lyra 所需的开发库。官方归档链接的 DIA 库从当前 Visual Studio 查找；自定义布置可传 `-DLYRA_DIA_GUIDS=<完整路径>`。
3. 用 vcpkg 从仓库的 `vcpkg.json` 安装固定版本的原生库，再从同一个 PowerShell 配置与验收。下面假设 `vcpkg`、用于宿主编译的 `clang++`、配套的 `clang-scan-deps` 和 `lld-link` 在 `PATH` 中；将 `$llvm` 改为实际解压目录：

   ```powershell
   $llvm = 'E:/tools/clang+llvm-18.1.8-x86_64-pc-windows-msvc'
   $ports = Join-Path $PWD 'build/windows-ports'
   vcpkg install --triplet x64-windows-static "--x-install-root=$ports"
   cmake --preset windows-release "-DLLVM_DIR=$llvm/lib/cmake/llvm" "-DCMAKE_PREFIX_PATH=$ports/x64-windows-static"
   cmake --build --preset windows-release
   ctest --preset windows-release
   cmake --install build/windows-release --prefix build/sdk-windows
   ```

   调试版改用 `windows-dev`。安装树包含标准库和静态库，并为 `import-c` 提供 `libclang` 与内建头文件；运行编译器仍需要 Visual C++ Redistributable，该环境通常由 VS Build Tools 提供。构建和运行源码的命令示例见[仓库说明](../README.md)。外部 C 静态库若也希望随应用单文件交付，使用 x64 MSVC ABI 和静态 CRT（`/MT`）。

### 打包与复验 SDK

在 Windows x64 构建机上用已通过测试的 `windows-release` 安装树打包：

```powershell
$name = 'lyra-0.1.0-alpha.1-windows-x64'
cmake --install build/windows-release --prefix "build/dist/staging/$name"
tar -a -cf "build/dist/$name.zip" -C build/dist/staging $name
```

在 Ubuntu 22.04 x86-64 构建机上用 `release` 安装树打包：

```sh
name=lyra-0.1.0-alpha.1-ubuntu22.04-x86_64
cmake --install build/release --prefix "build/dist/staging/$name"
tar -czf "build/dist/$name.tar.gz" -C build/dist/staging "$name"
```

解包后在对应平台从仓库根目录运行 `python tests/installed_cli.py --sdk <解包后的SDK目录>`。测试会搬迁安装树，在仓库外检查锁定源码包和 C ABI，并从解包的 SDK 构建、运行双包 JSONL 与 HTTP/SQLite 应用。Windows 需在 x64 Developer PowerShell 中执行，Linux 可用 `python3`。部署环境只运行生成的应用；Jammy 的独立运行验收见[部署验收脚本](../tests/jammy-smoke.sh)。VSIX 打包和桌面联调见[扩展说明](../editors/vscode/README.md#真实编辑器验收)。

Windows 的解包验收可加 `--deployment-bundle build/dist/windows-apps`，将两种构建的应用、测试数据库和验证脚本放进新目录。把该目录放到部署机，在目录里运行 `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\verify.ps1`。脚本只用系统 PowerShell，检查 JSONL 正常行与坏行的结果，以及 SQLite 服务的 `200` 和 `404` 响应。

### 开发机准备：WSL 2 + Ubuntu 24.04

只构建 Linux 版本时，在 WSL/Linux 中安装 LLVM 18.1 开发包和 Clang 18 即可，不需要另准备 Windows SDK。最低版本验收在 22.04 目标环境构建，再放入只装运行依赖的 22.04 镜像执行。

1. 在 PowerShell 运行 `wsl -l -v`。已有 `Ubuntu-24.04`（版本 2）就直接 `wsl -d Ubuntu-24.04`；新开发机没有该发行版时才运行 `wsl --install -d Ubuntu-24.04`，按系统提示完成首次启动。
2. 进入 **Ubuntu 终端**后，先运行 `clang-18 --version`、`llvm-config-18 --version --host-target --cmakedir` 和 `cmake --version`。头文件、库和 `LLVMConfig.cmake` 来自开发包，不能只验证 `clang`。缺包时才安装：

   ```sh
   sudo apt update
   sudo apt install build-essential clang-18 clang-tools-18 llvm-18-dev libclang-18-dev cmake ninja-build python3 libgc-dev libuv1-dev libsqlite3-dev libjansson-dev curl
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

构建和 CTest 使用仓库的 `dev` / `release` preset，见[仓库说明](../README.md)。非系统路径安装的 LLVM 可在配置时传 `-DLLVM_DIR="$(llvm-config-18 --cmakedir)"`。

## 编译器、Runtime 和 GC

LLVM 是代码生成后端，不是语言运行时或 GC。Lyra Runtime 实现任务调度、异常/诊断支持、对象布局与外部运行时协作；GC 算法优先选用维护成熟的现成实现，不自建第一版收集器。

原型使用成熟的非移动保守 GC（BDWGC），[任务与对象用例](../runtime/managed-tasks.md)验证异步帧保活、优化构建和原生工作线程注册。长期目标是精确根追踪。无论哪一步，异步 Task Frame、闭包、全局状态和已注册的 C 句柄中的存活引用都不能丢失；普通 C `malloc` 区域不得成为唯一活引用的无追踪存储位置。

语言不保证对象物理地址稳定，也不依赖终结器及时关闭外部资源。精确根追踪是后端目标，不等于必须采用移动式 GC。Safepoint、对象 ABI、FFI pin/handle 及 LLVM lowering 方案由后续原型确定。编译器和 Runtime 还须满足[跨域竞争时的内存安全底线](async-execution.md)；不能直接把语言允许的竞争降低为 LLVM 中可导致任意行为的普通并发读写。

## 首个 C ABI 用例：SQLite

显式 `extern` 声明绑定 C 标量、指针和不透明句柄。字符串按每个 C 接口的契约明确传长度或使用终止符；SQLite 文本在下一次 step 或 finalize 使原缓冲失效前复制为 Lyra 字符串。首个绑定是按需导入的官方 SQLite 包：打开连接、准备/执行语句、绑定值、迭代结果、确定性关闭 statement 和 connection。

数值类型按位宽与符号对应 C 的 `intN_t` / `uintN_t`，`Float32` 对应 `float`，`Float64` 对应 `double`。Linux 上使用浮点 `%` 的程序可能另依赖系统 `libm.so.6`；整数和不用该运算的程序无需引入它。

项目自己的 C 库可在 `lyra build`、`run`、`test` 上重复传 `--link /path/to/libexample.a`（也可传目标文件或共享库）。输入按命令行顺序交给 Clang，不执行包内脚本。`check` 只检查签名；链接阶段验证符号能否解析。`Int` 对应 `int64_t`，`Bool` 对应 `uint8_t`，`CPtr` 对应不透明指针；Lyra `String` 和 `Bytes` 都是带长度的运行时句柄，不是裸字节指针。使用共享库时要在目标机提供相同 ABI；`lyra.lock` 锁定 Lyra 源码包，不代为分发原生库。

用安装树的 `include/` 编译 C 适配器即可读取 `String`。例如，Lyra 声明 `extern fn writeText(text: String): Int;`，C 实现如下：

```c
#include <lyra/string.h>
#include <stdint.h>
#include <stdio.h>

int64_t writeText(const LyraString *text) {
    LyraStringView view = lyraStringView(text);
    return (int64_t)fwrite(view.data, 1, view.size, stdout);
}
```

`LyraString` 是不透明句柄。视图的 `data` 指向只读 UTF-8 字节；`size` 是字节数，包含内容中所有 NUL（即便 NUL 是最后一个字节），不计任何额外的结束符。不要读取 `data[size]` 或调用 `strlen`。空字符串返回非空 `data` 与零长度；普通 Lyra `String` 实参不会是空指针，C 显式调用 `lyraStringView(NULL)` 得到 `{NULL, 0}`。

`Bytes` 对应同一头文件里的不透明 `LyraBytes`。`lyraBytesView` 返回 `LyraBytesView`，其 `data` 是 `const unsigned char*`，不保证 UTF-8 或 NUL 结尾。C 返回 `String` 或 `Bytes` 时须返回相应的有效 Lyra 句柄（例如原样返回本次参数），不能返回视图、C 数组或 `NULL`。

编译器在同步 C 调用返回前保活所有传入的 `String` 和 `Bytes`，包括阻塞期间另一线程触发 GC 的情况。句柄和视图只在这次调用内借用，不能保存到全局变量或交给另一个线程；需要跨调用使用时，在返回前复制 `size` 个字节到 C 自己的存储。[安装验收](../tests/installed_cli.py)用搬迁后的头文件和独立的 C11 编译单元验证文本边界；[集合和文件验收](../tests/stdlib_daily_cli.py)验证字节句柄。其余运行时头文件不作为公共 C API 分发。

在异步任务中，可能阻塞的 SQLite 操作必须通过隔离的执行入口进行。首例可以在一次隔离的同步操作里打开数据库、执行查询并清理，再把数据值返回执行域。这样 `defer` 仍是同步的，也不会在调度线程上执行可能阻塞的关闭操作。

**已通过的端到端验收**：[原生句柄测试](../tests/runtime_sqlite.cpp)在系统 SQLite 的内存库中建表、写入 `7 / Ada` 并参数化查询；缺表时保留 `prepare` 的操作名、错误码和消息，关闭 statement、连接并触发 GC 后，拷出的文本仍有效。[Lyra 源码测试](../tests/sqlite_cli.py)还区分 SQL `NULL` 与空串，保留文本中的 NUL，并检查 `close()` 抛出 `Busy` 后可以继续用游标，释放游标后再经连接别名成功关闭。[真实 HTTP 服务](../tests/http_cli.py)在隔离线程执行查询，命中、无行、缺文件或缺表分别返回 200、404、500，并测到请求取消后的安全收尾。

**公开 API**：[std.sqlite](../std/sqlite/README.md) 的数据库操作直接返回 `Result<T, DbError>`。`next()` 的 `Ok(false)` 表示游标正常结束；列读取用 `Ok(None)` 表示 SQL `NULL`。`DbError` 保留 `kind`、`operation`、`code` 和 `message()`；[服务的查询库](../examples/http-sqlite/users/src/users.ly)用 `?` 传播为 `Result<User?, DbError>`，不再翻译原生状态。同步 `close()` 的 `Busy` 等清理失败走异常通道。

取消路径单独验收：用可控的测试门闩停住隔离操作，同域另一 Task 应仍能推进。取消拥有该操作的作用域时，**不能在原生操作还可能使用句柄时释放它**；放行并等它安全返回，清理后任务组才可退出。不要求不支持中断的 C 调用立即停止。这两条用例不用定时睡眠来猜调度顺序。

公开 `Connection` 是[普通资源引用](errors-and-resources.md)，可以传参、返回或存入对象；离开注册了 `defer close` 的块时仍照常关闭，带出引用不会延长原生资源的打开期。使用中或正在关闭的连接遇到另一次 `close()` 要报 `Busy`；关闭成功后再关则幂等。原始 C 指针、固定与释放留在不安全 FFI 边界；安全封装须遵守 SQLite 线程模式及 statement 的生命周期。

## 原生指针与闭包回调

`unsafe { ... }` 是词法块，正常结束取块尾值。下面的读写以字节为偏移、使用目标平台的字节序；支持全部定宽整数、`Float32/64` 及 `CPtr`，例如 `readUInt16`、`writeFloat64` 和 `readCPtr`。

```lyra
fn nativeValue(): Int32 {
    unsafe {
        val buffer = CPtr.alloc(8)
        defer buffer.free()
        buffer.offset(4).writeInt32(0, 42)
        buffer.readInt32(4)
    }
}
```

`CPtr.alloc(bytes)` 使用 C `malloc`，内存未初始化，负长度或分配失败抛 `Exception`。`free()` 只能释放同一 C 分配器给出的基址，不能释放借用指针或偏移后的地址。边界、对齐、生命周期和线程同步由调用者负责；原生缓冲区不会被 GC 扫描。`CPtr.nullPtr()`、`ptr.isNull()` 和指针相等比较可在 `unsafe` 外使用。违例的指针操作在调用名处报 `LYF0002`。

`CCallback.register(closure, fallback: value)` 登记同步闭包，并用 GC 根保活捕获环境，直到显式注销。C 端用 `function()` 取得函数地址、用 `context()` 取得第一个实参；其余参数和返回值按上述 C ABI 传递。比如 Lyra `(Int32) -> Int` 对应 `int64_t (*)(void *context, int32_t value)`。首版回调支持数值、`Bool`、`CPtr`，返回 `Unit` 时对应 `void` 且不接收 fallback。

- 回调在发起调用的原生线程同步执行。若线程尚未接入 GC，trampoline 在调用前注册线程、返回前注销。不会为它启动 Task 或切换执行域。
- 普通异常先在 Lyra 内完成清理，再由 trampoline 截获。非 `Unit` 回调向 C 返回登记时的 fallback；`Unit` 正常返回。`takeError(): Option<Exception>` 取走自上次读取以来第一条未收取的异常，保留原异常及其位置；并发失败以最先写入者为准。
- 先让 C 端移除登记并等所有已进入的回调返回，再在 `unsafe` 中调用 `close()`；不得从仍在执行的同一回调中注销。关闭幂等、释放 GC 根，关闭后 `function()` 和 `context()` 返回空指针；未收取的异常仍可读取。C 不得继续使用关闭前取得的任一地址。`CCallback` 与其他资源句柄一样不提供默认 Hash。

## 从 C 头文件生成绑定

```sh
lyra import-c include/api.h -I include -D API_VERSION=2 -o src/bindings.ly
lyra build . --link /path/to/libapi.a
```

命令使用 SDK 内的 Clang 按应用目标解析 C17 头文件。`-I` 和 `-D` 可重复；内建头文件提供 `stdint.h`、`stddef.h` 等，第三方头文件和完整系统头文件由调用方通过包含目录提供。导入只为所选头文件中的外部函数生成 `pub extern fn`，不会为间接包含的头文件生成绑定。整数位宽按目标确定，例如 Windows 的 `long` 是 32 位、Linux 是 64 位；C 指针（包括函数指针）映射为 `CPtr`，`_Bool` 映射为 `Bool`。不生成 C 实现，不链接 C 库，也不在普通构建中自动运行。

按值聚合类型、C 全局变量、C++、可变参数、非默认调用约定与 inline 函数不在首版映射范围内。无法映射的声明报带头文件位置的 `LYF0004` 警告，仍输出其余绑定；C 解析错误报 `LYF0003` 并停止生成。可通过 `--diagnostics=json` 获取诊断。省略 `-o` 输出到 stdout，`-o` 只创建新文件；可从生成的普通 Lyra 源码继续手写 `extern` 或包装 API。

## 标准库与诊断

- `async`、`await` 等影响 Task 控制流的能力由语言和 Runtime 配合支持；核心 Runtime 和基础库随工具链，HTTP、JSON/TOML 作为官方维护、按需导入的库包，可独立发修复版。首个编译器切片的正反例、入口与后续验证顺序见[编译器验收设计](compiler-acceptance.md)。
- 未捕获异常诊断应包含源码位置、错误原因及异步调用链；用户应能从 `await` 返回的失败看到任务被启动和挂起的位置。
- 前端的 `check`/类型诊断在 LLVM 代码生成前完成；输出稳定错误码、主源码范围、原因、相关类型或调用点，支持机器可读 JSON。测试断言类别和源码范围，不锁定自然语言文案或 LLVM 内部报错。

## 已接受验收行为

- 原生编译生成本平台 ELF 或 PE，不要求安装 Lyra VM 或 LLVM Runtime；动态库依赖明确可查。
- WSL/Linux 构建的产物在声明的目标 Linux 环境中执行。
- 同步 SQLite 调用不阻塞串行异步执行域 Worker；SQLite 的外部资源可确定性关闭。
- 任务暂停期间仅由异步状态帧持有的对象仍能通过 GC 压力测试。
- 异步异常报告能沿源调用链回溯到 `await` 和原始任务创建位置。
- 有/无动态 SQLite 依赖的交付行为显式区分；未来支持 scratch 时通过独立目标配置声明。
- [FFI 命令行验收](../tests/ffi_cli.py)链接独立 C 库，在 Debug/Release 应用中核对定宽读写、外部线程回调期间的 GC 保活、异常隔离和注销；用搬迁后的 SDK 在清空 `PATH` 的环境下导入头文件，再链接运行生成的绑定。

## 仍开放

- 额外架构、libc/链接器组合及全静态支持矩阵。
- 后续收集器的选择、精确根追踪与优化级别的交互，以及是否在回调以外开放通用外部线程注册。
- Runtime 默认管理 GC 和 Worker，并提供基本限制和统计；具体配置、指标和默认值由原型测量。
- `extern` 声明的完整 ABI 类型表、错误码到 Result 的具体映射、内部句柄表示和中断接口。
- 官方库包与工具链的版本兼容范围。
