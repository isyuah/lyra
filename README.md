# Lyra

Lyra 是一门使用 LLVM AOT 的编译型语言。编译器把通过静态检查的源码编译成 Windows x64 的 PE 或 Linux x86-64 的 ELF 可执行文件，部署应用不需要 Lyra 或 LLVM。语言设计和验收契约见 [RFC 索引](rfc/README.md)，当前可以运行的语法见下文。

**0.1 Alpha 的使用范围：** Windows x64 和 Linux x86-64 上的单机 CLI、可发布的源码库，以及使用 SQLite 的小型 HTTP/HTTPS 服务。语言和包格式仍处于开发期。

教程源码在 [`docs/`](docs/)（Astro Starlight）；本地构建和内容约定见 [`docs/README.md`](docs/README.md)。

## 安装或更新 SDK

Windows x64 和 Linux x86-64 都可以用安装脚本获取最新发布版。脚本先校验 `SHA256SUMS`，再校验 SDK 内部文件清单；升级前先暂存旧版本，安装失败时恢复。若旧目录含有用户文件或修改过的 SDK 文件，会保留旧目录备份。无需管理员权限。

**Windows x64：** 在 PowerShell 中下载并运行：

```powershell
$installer = Join-Path $env:TEMP 'lyra-install.ps1'
Invoke-WebRequest 'https://raw.githubusercontent.com/isyuah/lyra/main/install.ps1' -OutFile $installer
& $installer
```

脚本会复用 `PATH` 中的已有 SDK；未检测到时安装到 `%LOCALAPPDATA%\Programs\Lyra` 并添加到当前用户的 `PATH`。已有 SDK 不在 `PATH` 时，传入它的目录，例如 `& $installer -InstallDir 'E:\Softw\lyra'`。更新前应关闭正在运行的 Lyra 语言服务和 CLI。

**Ubuntu 22.04+ x86-64：** 在 Shell 中运行：

```sh
curl -fsSL https://raw.githubusercontent.com/isyuah/lyra/main/install.sh | sh
```

脚本会复用 `PATH` 中能识别的 SDK；未检测到时安装到 `~/.local/opt/lyra`，并在 `~/.local/bin/lyra` 创建稳定入口。若 `~/.local/bin` 不在 `PATH`，脚本会打印需要添加的配置。要指定目录，先下载脚本再传 `--install-dir`：

```sh
curl -fsSL https://raw.githubusercontent.com/isyuah/lyra/main/install.sh -o /tmp/lyra-install.sh
sh /tmp/lyra-install.sh --install-dir "$HOME/.local/opt/lyra"
```

安装脚本需要 `curl`、`tar` 和 `sha256sum`。

两端脚本也可通过 GitHub Actions 的 **package_sdk** 手动构建任务下载；Release 同时提供脚本和原始压缩包。需要离线安装时，可下载对应 SDK 归档并手动校验发布页中的 `SHA256SUMS`。

## 用预编译 SDK 新建项目

从 [GitHub Releases](https://github.com/isyuah/lyra/releases) 下载对应平台的 SDK；发布页的 `SHA256SUMS` 可校验归档。

**Windows x64：** 解压 Lyra SDK 即可，在普通 PowerShell 中运行。无需安装 Clang、LLVM、VS Build Tools 或 Visual C++ Redistributable，也不需要 Developer PowerShell：

```powershell
Expand-Archive lyra-0.1.0-alpha.1-windows-x64.zip -DestinationPath .
$lyra = Resolve-Path 'lyra-0.1.0-alpha.1-windows-x64/bin/lyra.exe'
& $lyra new hello
& $lyra lock hello --offline
& $lyra test hello --locked --offline
& $lyra run hello --release --locked --offline
```

**Ubuntu 22.04 x86-64：** 解压 Lyra SDK 即可，无需另装 LLVM、Clang、链接器或 SQLite 开发库。发布包以 Ubuntu 22.04 的 glibc 为基线；`bin/lyra` 启动器会为编译器设置 SDK 内 LLVM 库的搜索路径：

```sh
tar -xzf lyra-0.1.0-alpha.1-ubuntu22.04-x86_64.tar.gz
export PATH="$PWD/lyra-0.1.0-alpha.1-ubuntu22.04-x86_64/bin:$PATH"
lyra new hello
lyra lock hello --offline
lyra test hello --locked --offline
lyra run hello --release --locked --offline
```

测试运行项目自带的 `tests/answer.ly`，最后一条命令打印 `42`。搬迁时保留整个 SDK 目录；部署时只需分发 `hello/build/release/app.exe`（Windows）或 `hello/build/release/app`（Linux）。本地项目和已缓存的依赖可离线构建；获取远程包由 Lyra SDK 自带下载器完成，不要求宿主 `curl`。Lyra 应用使用 HTTPS 时走 SDK 的静态库，不运行 `curl` 或 `openssl` 命令；客户端默认使用系统信任根（Linux 通常由 `ca-certificates` 提供），也可以显式提供 PEM。[原生工具链 RFC](rfc/native-toolchain.md#打包与复验-sdk)提供打包和验收命令。

## 从源码构建

**Windows x64：** 按 [Windows 开发机配置](rfc/native-toolchain.md#开发机准备windows-x64)准备宿主 Clang、LLVM 18.1、VS Build Tools 和 LLVM-MinGW UCRT 目标库。在 x64 Developer PowerShell 中配置并构建 `windows-dev` 或 `windows-release`，无需 WSL。构建完成后可以直接运行：

```powershell
$lyra = Resolve-Path 'build/windows-release/bin/lyra.exe'
& $lyra new '中文 项目' --name hello
& $lyra lock '中文 项目' --offline
& $lyra test '中文 项目' --locked --offline
& $lyra build '中文 项目' --release --locked --offline
& './中文 项目/build/release/app.exe'
```

最后一条命令输出 `42`。用 `cmake --install build/windows-release --prefix build/sdk-windows` 生成可搬迁 SDK；用这个 SDK 编译 Lyra 项目不需要源码构建机的工具。

完整 HTTPS 测试需要开发机上的 `openssl` 命令来生成临时证书。若该命令不在 `PATH`，在运行 `ctest --preset windows-dev` 或 `ctest --preset windows-release` 前，把 `LYRA_TEST_OPENSSL` 设为实际路径，例如 Git for Windows 安装目录下的 `usr/bin/openssl.exe`。不需要信任测试证书或修改系统证书库。

**Linux x86-64：** 在 Linux 的仓库根目录执行；在 Windows 上给 Linux 服务器构建应用时使用 WSL 2。源码构建使用 C++23 命名模块，需要 Clang/LLVM 18.1、配套的 `clang-scan-deps-18`、`libclang-18-dev`、`lld-18`、CMake 3.28+、Ninja 1.11+ 和 Python 3（测试）。

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
build/dev/bin/lyra check examples/compiler-smoke/fail --diagnostics=json
build/dev/bin/lyra build examples/compiler-smoke/pass --release
examples/compiler-smoke/pass/build/release/app
```

`check .../fail` 预期非零退出，在 `;` 处报告 `LYT0001`；最后一条命令应打印 `3` 并正常退出。要构建优化版编译器，把三个 preset 命令中的 `dev` 换成 `release`。用 `cmake --install build/dev --prefix <目录>` 安装工具链，搬迁时整体复制前缀，包含 `bin/`、`lib/`、`libexec/`、`share/` 和给 C 适配器使用的 `include/`。

源码构建需要 `libgc-dev`、`libuv1-dev`、`libsqlite3-dev`、`libjansson-dev`、`zlib1g-dev` 和 `libssl-dev`（OpenSSL 3），完整 HTTPS 测试另用 `openssl` 命令。CMake 下载并校验固定版本的 llhttp、tomlc17、curl、c-ares 和 nghttp2。离线配置可分别传 `-DLYRA_LLHTTP_SOURCE_DIR=...`、`-DFETCHCONTENT_SOURCE_DIR_LYRA_TOMLC17=...`、`-DFETCHCONTENT_SOURCE_DIR_LYRA_CURL=...`、`-DFETCHCONTENT_SOURCE_DIR_LYRA_CARES=...` 和 `-DFETCHCONTENT_SOURCE_DIR_LYRA_NGHTTP2=...`，指向已准备的源码。nghttp2 固定为 1.70.0。部署**编译好的应用**不用带工具链或这些开发库。

维护者可用 `python tools/package_sdk.py build/windows-release` 或 `python3 tools/package_sdk.py build/release` 打包已构建的 SDK。脚本安装到新的临时前缀，校验文件清单，在 `build/dist/` 生成带目标和 Git 版本的制品，并附文件 SHA-256 清单；同名制品已存在时不会覆盖。Linux 的 Ubuntu 22.04 发行包应在 Ubuntu 22.04 环境构建。

### 试跑一个完整项目

从仓库根目录执行。下面先原生编译并运行库的 Lyra 测试，再运行依赖它的 CLI；不需要启动包中心。

```sh
build/dev/bin/lyra test examples/jsonlines/metrics --locked --offline
printf '%s\n' '{"station":"东区","delta":7}' '{"station":"西区","delta":-2}' |
  build/dev/bin/lyra run examples/jsonlines/stats --release --locked --offline
```

输出是 `{"rows":2,"total":5,"rejected":0}`。CLI 还支持文件输入、TOML 默认值和命令行覆盖。想做服务，可按 [HTTP/SQLite 示例](examples/http-sqlite/README.md)建库，通过 HTTP 或 HTTPS 新增并查询用户。常用模块和各自的 API 示例见[标准库索引](std/README.md)。

## 发布 SDK

在 `main` 提交 `VERSION` 后，推送同名的 `v` 前缀 tag。例如当前版本是 `0.1.0-alpha.1`：

```sh
git tag -a v0.1.0-alpha.1 -m "Lyra 0.1.0-alpha.1"
git push origin v0.1.0-alpha.1
```

GitHub Actions 校验 tag 和该提交的 `VERSION` 一致，在 Windows x64 与 Ubuntu 22.04 x86-64 分别验收 Debug、Release 和可搬迁 SDK，并测试两端的安装与覆盖升级；全部通过后才将安装脚本、Windows ZIP、Ubuntu TAR.GZ 和 `SHA256SUMS` 发到同一 GitHub Release。带预发布后缀的版本标记为 Pre-release。分支和 PR 只跑验收；在 Actions 手动选择 `package_sdk` 可验证出包并下载临时制品，不会发布。

Windows CI 固定使用 Clang 23.1.6 构建 Lyra，避免依赖 Runner 自带的 Clang；SDK 内仍使用 LLVM 18.1.8。

## 命令与语言子集

```text
lyra --version
lyra new <目录> [--name <包名>] [--lib]
lyra check|build|run|test <包目录> [--locked] [--offline] [--diagnostics=json]
lyra doc <包目录> [--locked] [--offline] [-o <文件>]
lyra doc --stdlib [-o <文件>]
lyra build <包目录> [-o <产物>] [--release] [--debug-info]
lyra run <包目录> [--release] [-- <程序参数>...]
lyra lock|fetch <目录> [--locked] [--offline]
lyra pack <目录> [-o <源码包>|--registry <本地目录>]
lyra lsp --stdio
lyra fmt [--check] <文件.ly>...
```

- `lyra.json5` 包中一个目录组成一个模块。`import self.net` 只在本文件绑定 `net`；`import cfg.net as remote` 可换名，写 `remote.port()`。用 `import cfg.net.{Endpoint, port as remotePort}` 才引入裸名，`pub import cfg.net.{Endpoint}` 可重导出。无清单的单模块冒烟样例只读取 `src` 直属文件。库包可独立 `check`，无需 `main`。详见[模块 RFC](rfc/modules-packages.md)。
- `test` 编译并运行每个 `tests/*.ly` 中的 `fn main(): Unit`，用 `import self as app` 或 `import self.{名称}` 访问包的公开 API。workspace 根支持一次检查或测试多个成员。锁、缓存、只读 HTTP(S) 源和脚本边界见[包工具手册](compiler/packages.md)。
- `doc` 从编译后的公开接口生成参考数据，标准库模式还包含内置 API 和语法速查。说明和例子只需写在声明前的 `///` 中；[JSON 接入和注释约定](compiler/reference.md)适用于自建文档站。
- 支持有符号和无符号整数 `Int8/16/32/64`、`UInt8/16/32/64`（`Int`、`UInt` 为 64 位别名）、`Float32`、`Float64`（`Float`）、`Bool` 和 `Unit`。提供数值四则、余数、比较和复合赋值；整数溢出或除零在 Debug/Release 都带源码位置报错。窄化可用 `value.toInt8Checked()` 等方法，返回 `Option`。
- 支持具名形参、按源码顺序求值的命名实参、`val` / `var`、嵌套作用域、短路 `&&` / `||`、`if` / `else`、`while`、`break`、`continue` 和 `return`。普通块以末表达式为值，空块或带末尾分号的块取 `Unit`。
- [文本与字节](rfc/language-core.md#文本与字节)：`String` 支持 UTF-8、拼接和 `"Hello, ${name}"` 插值；单引号给出 Unicode `Char`，`b"\xFF"` 给出不可变 `Bytes`，可显式检查和转换为文本。
- `struct` 按值复制，`class` 保留对象身份，可定义字段、主构造和方法。`gcCollect()` 可主动收集。对象由 BDWGC 管理；编译器安装目录带静态 GC 和 libuv 归档，部署应用不需要安装这两个库。见[对象验收程序](tests/fixtures/objects/src/main.ly)。
- [实例方法](rfc/language-core.md#实例方法与静态方法)不用声明接收者：`fn label(): String { name }` 可直接读字段，同名参数遮蔽时写 `this.name`。类函数声明为 `static fn`，用 `User.make(...)` 调用，无需对象实例。
- 字段可以写 `pub val name: String? = "guest"`。省略构造实参或解码缺少该键的 JSON 对象时都会求同一个默认值。显式传入 `None` 或解码 JSON 的 `null` 都会保留空值。
- 泛型函数 `fn identity<T>(value: T): T { value }` 可写 `identity(42)` 或 `identity<String>("Ada")`。Struct、Class 和方法也可带类型形参，例如 `class Cell<Item>(var value: Item)` 与 `Cell("Ada")`。未调用的泛型也检查函数体，具体类型组合各自生成原生实现。
- `fn draw<T: Render & Identifiable>(item: T)` 可调用两个 trait 要求的方法。业务类型有可见、签名匹配的方法即可，不用声明 `implements`。trait 可声明 `type Item`，实现类型提供 `pub type Item = String`，泛型中用 `T.Item` 引用。见[泛型与接口](rfc/generics-and-interfaces.md)。
- [函数值与闭包](compiler/closures.md)可以返回、传参或存入集合。`apply(4) { it * 2 }` 把尾块传给末尾的固定函数形参；闭包共享被捕获的绑定，跨 `await`、GC 后仍可调用，`return` 和 `?` 只退出最近的 lambda。
- [列表和变长参数](compiler/collections.md)支持 `[1, 2]`、按下标读写、`.size`、`.add(value)`、参数默认值、`fn sum(values: Int...)`。`sum(...ids)` 展开元素；函数收到本次调用自己的列表，更改槽位不影响调用方。如果元素是对象引用，引用的仍是同一个对象。
- `Option<T>`（简写 `T?`）和 `Result<T,E>` 支持嵌套和 `?` 提前返回；`map`、`flatMap`、`orElse`、`unwrap`、`expect` 和[显式互转](std/core/README.md)无需导入。可用 [`enum`](rfc/language-core.md#封闭枚举与匹配) 定义带位置或命名载荷的分支。`match` 要求穷尽，支持嵌套解构、`A(x) | B(x)`、整数范围和 `Some(x) if x > 0 => x` 形式的守卫。
- `for value in values` 遍历列表，或通过可见的同步 `iterator()`、`next(): Option<T>` 遍历自定义集合。整数循环可写 `for i in 0..<count`（不包含 `count`）或 `for i in 1..count`（包含 `count`）。`user?.name ?? "unknown"` 只在接收者有值时访问成员，只在结果为空时计算兜底表达式。
- [std.collections](std/collections/README.md) 为 `List` 和 `FrozenList` 提供 `map`、`filter`、`fold`、`any`、`find`、稳定 `sortedBy`；导入对应的 `ListOps` / `FrozenListOps` 可链式调用。两种列表都内置 `getOrNone`、`firstOrNone` 和 `lastOrNone`。`HashMap` 和 `HashSet` 可直接用 `for` 遍历进入循环时的快照。
- 在源或目标类型的定义模块写 `implicit (user: User): Response { ... }`，即可在需要 `Response` 时直接传入或返回 `User`。不同具体错误之间的 `?` 也用同一种直接转换；不额外声明 trait、不串接转换，两端各有一条可见转换时在使用处报歧义。
- `async fn` 调用返回冷 `Task<T>`；`await` 在需要时启动，重复等待复用结果，`spawn` 在同一串行域调度。`await blocking { ... }` 把同步工作交给有界线程池，不占住当前域；跨挂起点存活的局部值由 GC 扫描。`async fn main(): Result<Unit,String>` 的 `Err` 打印到 stderr 并以非零状态退出。见[异步实现说明](runtime/managed-tasks.md)。
- `taskGroup` 等待孩子收尾；`parallel` 和 [`parallelFor`](std/collections/README.md#有界并行遍历) 可在多个 OS 线程上同时执行。用 `LYRA_PARALLEL_WORKERS` 调整 CPU Worker 数，复合共享更新可用 [std.sync](std/sync/README.md) 的锁和原子操作。
- 普通异常写 `throw Exception("reason")`，用 `try { ... } catch e: Exception { ... }` 或 `catch { ... }` 捕获。`defer` 在退出当前块时倒序清理；取消独立于异常。使用 [std.task](std/task/README.md) 给一棵任务树设置 trace 或 deadline，不需要逐层传 `context`。
- `extern fn` 通过 C ABI 调用原生函数。`--link` 接受 Linux 的 `.o`、`.a`、`.so`，或 Windows x64 COFF 的 `.obj`、`.lib`、`.a`。Windows C 适配器推荐用 LLVM-MinGW UCRT 构建；依赖私有 MSVC CRT 符号的库通过 DLL 的 C 接口调用。`<lyra/string.h>` 可按字节读取 `String`。[std.io](std/io/README.md) 可读参数、标准流和文件；[std.sqlite](std/sqlite/README.md) 提供预编译语句，两端均按需静态链接 SQLite。
- `lyra import-c api.h -I include -o src/bindings.ly` 用 SDK 自带的 Clang 解析 C 头文件并生成 `extern` 声明。`unsafe` 块支持定宽指针读写和同步闭包回调；[FFI RFC](rfc/native-toolchain.md#原生指针与闭包回调)列出生命周期和 ABI 契约。
- [清单的 `native` 节](compiler/packages.md#随包分发-c-库)可随包分发 C 源码和目标静态库。SDK 自动编译并链接传递依赖，不要求在开发机额外安装 C 编译器。
- [std.json](std/json/README.md) 支持显式 `#derive(Json)`，派生器本身是 [Lyra 宏](std/meta/README.md)。用 [std.http](std/http/README.md) 可以运行[真实 HTTP/SQLite 查询服务](examples/http-sqlite/README.md)，无需 VM 或外部 Web 服务器。
- [std.toml](std/toml/README.md) 同样用显式 `#derive(Toml)` 生成 TOML 编解码器，严格拒绝未知字段，可用显式入口放宽；别名、默认值、嵌套表和数组表均有端到端验收。日期时间目前解析为 TOML 专用值，业务类型需要手写映射。
- `const` 可调用纯同步函数并预计算复合值，`pub const` 可跨包导入。编译期求值范围与预算见[元编程 RFC](rfc/metaprogramming-data.md#编译期常量)。[std.reflect](std/reflect/README.md) 可显式读取 `Any` 的类型名及公开 Struct/Class 字段；它与静态 JSON/TOML 派生相互独立。
- 形参和字段可带 `[path]`、`[query("tag")]` 等编译期标记；声明宏据此生成包装函数。表达式宏 `#name(...)` 可以校验显式授予的快照，再把调用者实参拼进普通的类型检查；无需运行时反射或联网。
- [JSON Lines 统计 CLI](examples/jsonlines/README.md) 是可直接运行的双包项目：公开库解码，命令行逐行统计，错误带行号。验收会实际发布库、移走源码和中心，再从锁和缓存重新原生编译。
- `check` 不生成原生产物。`build` 由 LLVM 直接生成目标文件，再用 SDK 内的 LLD 链接。默认输出 `<包目录>/build/debug/app`（`-O0`，含源码级调试信息），`--release` 输出 `build/release/app`（`-O2`），`--release --debug-info` 保留优化版符号。Windows 的 `app.exe` 记录了配套 `.pdb` 的文件名，两者放在同一目录即可调试。目标文件按 LLVM IR、后端版本和发射选项缓存；每次仍进行类型检查与链接。缓存默认位于 `.lyra-cache/objects`，`LYRA_CACHE` 可指定共享缓存根。`-o` 指定完整目标路径；类型错误或链接失败不会覆盖已有产物。
- 设置 `LYRA_NATIVE_BACKTRACE=1` 会在未捕获异常的 Lyra 源码链后显示原生帧。见[调试原生程序](rfc/native-toolchain.md#调试原生程序)。
- `lyra check` 和 `lyra lsp --stdio` 共用诊断恢复，可一次报告独立函数中的多个错误。VS Code 支持导入和分号 Quick Fix、语义高亮、大纲、工作区符号搜索、定义和类型定义，以及未保存的 `lyra.json5` 补全与校验。完整功能和更新方法见[扩展说明](editors/vscode/README.md)；服务端不会下载包或执行准备脚本。LSP 行列使用 UTF-16，CLI 诊断保留 UTF-8 字节范围。
- `lyra fmt` 整理显式传入的 `.ly` 文件，`--check` 只检查不写入。首版整理缩进和 token 之外的行尾空白，不重排语句或改动字面量、注释内容和原有换行；无需解析包或展开宏。
- 诊断写到标准错误；`--diagnostics=json` 输出诊断数组，成功为 `[]`。带源码位置时，`range.start/end` 是左闭右开区间：`offset` 为从 0 开始的原文件 UTF-8 字节偏移，`line/column` 从 1 开始，列按 Unicode 码点计，CRLF 和 LF 均算一次换行。

## 验证 Ubuntu 22.04 部署

在仓库根目录运行 `docker build --platform linux/amd64 -f tests/jammy.Dockerfile --target verify .`。构建阶段在 Ubuntu 22.04 上编译 Debug、Release 并运行 CTest；一个精简镜像用搬迁后的 SDK 编译和运行应用，另一个只运行独立产物，两者的 `openssl` 命令仅作为测试探针。应用验收覆盖 JSONL 标准输入和文件、TOML 配置和 CLI 覆盖、HTTPS 写入 SQLite 后读回，以及校验证书的出站 HTTPS。`readelf -dW` 检查整数、对象、标准 I/O 和 JSONL 只动态依赖 `libc.so.6`，SQLite 与 HTTPS/SQLite 服务另用系统 `libm.so.6`；SQLite 和 TLS 库不以动态依赖交付。
