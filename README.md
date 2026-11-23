# Lyra

Lyra 是一门使用 LLVM AOT 的编译型语言。编译器把通过静态检查的源码编译成 Linux 可执行文件，不需要在部署机安装 LLVM。语言设计和验收契约见 [RFC 索引](rfc/README.md)，当前可以运行的语法见下文。

**0.1 Alpha 的使用范围：** x86-64 Linux 上的单机 CLI、可发布的源码库，以及使用 SQLite 的小型 HTTP 服务。可以从下面的项目开始；语言和包格式仍处于开发期。

## 从源码构建

在 **x86-64 Linux** 的仓库根目录执行；Windows 开发机先进入 WSL 2 的 Ubuntu 24.04。编译器使用 C++23 命名模块，需要 Clang/LLVM 18.1、配套的 `clang-scan-deps-18`、CMake 3.28+、Ninja 1.11+ 和 Python 3（测试）。安装步骤见[原生工具链 RFC](rfc/native-toolchain.md)。

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
build/dev/bin/lyra check examples/compiler-smoke/fail --diagnostics=json
build/dev/bin/lyra build examples/compiler-smoke/pass --release
examples/compiler-smoke/pass/build/release/app
```

`check .../fail` 预期非零退出，在 `;` 处报告 `LYT0001`；最后一条命令应打印 `3` 并正常退出。要构建优化版编译器，把三个 preset 命令中的 `dev` 换成 `release`。用 `cmake --install build/dev --prefix <目录>` 安装工具链，保留安装树的 `bin/`、`lib/`、`share/lyra/std/`。首次构建需要构建机上的 GC、libuv、SQLite、Jansson 开发包；CMake 下载并校验固定版本的 llhttp，已有源码时可设 `-DLYRA_LLHTTP_SOURCE_DIR=...` 离线配置。部署**编译好的应用**只需复制产物到约定的 Linux 环境，不用带工具链。

### 试跑一个完整项目

从仓库根目录执行。下面先原生编译并运行库的 Lyra 测试，再运行依赖它的 CLI；不需要启动包中心。

```sh
build/dev/bin/lyra test examples/jsonlines/metrics --locked --offline
printf '%s\n' '{"station":"东区","delta":7}' '{"station":"西区","delta":-2}' |
  build/dev/bin/lyra run examples/jsonlines/stats --release --locked --offline
```

输出是 `{"rows":2,"total":5,"rejected":0}`。想做服务，可按 [HTTP/SQLite 示例](examples/http-sqlite/README.md)建库，发送一次真实的 `GET /users/7`。想新建自己的项目，使用 `lyra new <目录>`，然后 `lyra test <目录>`、`lyra run <目录>`。

## 命令与语言子集

```text
lyra new <目录> [--name <包名>] [--lib]
lyra check|build|run|test <包目录> [--locked] [--offline] [--diagnostics=json]
lyra build <包目录> [-o <产物>] [--release]
lyra run <包目录> [--release] [-- <程序参数>...]
lyra lock|fetch <目录> [--locked] [--offline]
lyra pack <目录> [-o <源码包>|--registry <本地目录>]
```

- `lyra.json5` 包中一个目录组成一个模块。`import self.net.port` 从本包导入，`import cfg.defaultPort as port` 从清单中简称为 `cfg` 的库导入。默认私有，`pub import` 可重导出。无清单的单模块冒烟样例只读取 `src` 直属文件。库包可独立 `check`，无需 `main`。
- `test` 编译并运行每个 `tests/*.ly` 中的 `fn main(): Unit`，用 `import self.<名称>` 访问包的公开 API。workspace 根支持一次检查或测试多个成员。锁、缓存、只读 HTTP(S) 源和脚本边界见[包工具手册](compiler/packages.md)。
- 支持 `Int`、`Bool`、`Unit`、具名形参、按源码顺序求值的命名实参、`val` / `var`、嵌套作用域、赋值和复合赋值、整数四则与余数、比较、短路 `&&` / `||`、`if` / `else`、`while`、`break`、`continue` 和 `return`。整数溢出或除零在 Debug/Release 都带源码位置报错。普通块以末表达式为值，空块或带末尾分号的块取 `Unit`。
- `String` 支持 UTF-8、拼接和内容相等；`struct` 按值复制，`class` 保留对象身份，可定义字段、主构造和方法。`gcCollect()` 可主动收集。对象由 BDWGC 管理；编译器安装目录带静态 GC 和 libuv 归档，部署应用不需要安装这两个库。见[对象验收程序](tests/fixtures/objects/src/main.ly)。
- 泛型函数 `fn identity<T>(value: T): T { value }` 可写 `identity(42)` 或 `identity<String>("Ada")`。未调用的泛型也检查函数体，已调用的具体类型组合各生成一份原生实现。
- [列表和变长参数](compiler/collections.md)支持 `[1, 2]`、按下标读写、`.size`、`.add(value)`、参数默认值、`fn sum(values: Int...)`。`sum(...ids)` 展开元素；函数收到本次调用自己的列表，更改槽位不影响调用方。如果元素是对象引用，引用的仍是同一个对象。
- `Option<T>`（简写 `T?`）和 `Result<T,E>` 支持嵌套、穷尽 `match` 和 `?` 提前返回。`None`、`Some(None)` 和 `Some(Some(value))` 是不同的值。
- `for value in values` 遍历列表；`user?.name ?? "unknown"` 只在接收者有值时访问成员，只在结果为空时计算兜底表达式。
- 在源或目标类型的定义模块写 `implicit (user: User): Response { ... }`，即可在需要 `Response` 时直接传入或返回 `User`。不同具体错误之间的 `?` 也用同一种直接转换；不额外声明 trait、不串接转换，两端各有一条可见转换时在使用处报歧义。
- `async fn` 调用返回冷 `Task<T>`；`await` 在需要时启动，重复等待复用结果，`spawn` 在同一串行域调度。`await blocking { ... }` 把同步工作交给有界线程池，不占住当前域；跨挂起点存活的局部值由 GC 扫描。`async fn main(): Result<Unit,String>` 的 `Err` 打印到 stderr 并以非零状态退出。见[异步实现说明](runtime/managed-tasks.md)。
- 普通异常写 `throw Exception("reason")`，用 `try { ... } catch e: Exception { ... }` 或 `catch { ... }` 捕获。`defer` 在退出当前块时倒序清理；取消独立于异常。使用 [std.task](std/task/README.md) 给一棵任务树设置 trace 或 deadline，不需要逐层传 `context`。
- `extern fn` 通过 C ABI 调用原生函数。编译自己的 C 适配器时，从工具链 `include/` 引入 `<lyra/string.h>` 即可按字节读取 `String`，部署应用不用带头文件。随工具链安装的 [std.io](std/io/README.md) 可读参数、标准流和文件；[std.sqlite](std/sqlite/README.md) 可用预编译语句查询系统 SQLite。只有使用 SQLite 的应用才需要目标机提供 `libsqlite3.so.0`。
- [std.json](std/json/README.md) 支持显式 `#derive(Json)`，派生器本身是 [Lyra 宏](std/meta/README.md)。用 [std.http](std/http/README.md) 可以运行[真实 HTTP/SQLite 查询服务](examples/http-sqlite/README.md)，无需 VM 或外部 Web 服务器。
- 形参和字段可带 `[path]`、`[query("tag")]` 等编译期标记；声明宏据此生成包装函数。表达式宏 `#name(...)` 可以校验显式授予的快照，再把调用者实参拼进普通的类型检查；无需运行时反射或联网。
- [JSON Lines 统计 CLI](examples/jsonlines/README.md) 是可直接运行的双包项目：公开库解码，命令行逐行统计，错误带行号。验收会实际发布库、移走源码和中心，再从锁和缓存重新原生编译。
- `check` 不调用 Clang、不创建产物。`build` 默认输出 `<包目录>/build/debug/app`（`-O0`），`--release` 输出 `build/release/app`（`-O2`），`-o` 指定其他路径。类型错误或链接失败都不会覆盖已有产物。
- 诊断写到标准错误；`--diagnostics=json` 输出诊断数组，成功为 `[]`。带源码位置时，`range.start/end` 是左闭右开区间：`offset` 为从 0 开始的原文件 UTF-8 字节偏移，`line/column` 从 1 开始，列按 Unicode 码点计，CRLF 和 LF 均算一次换行。

## 验证 Ubuntu 22.04 部署

在仓库根目录运行 `docker build --platform linux/amd64 -f tests/jammy.Dockerfile --target verify .`。构建阶段在 Ubuntu 22.04 上编译工具链、运行 CTest，生成 Debug 与 Release 的应用；运行阶段将应用复制到新的 `ubuntu:22.04` 执行。整数、对象、标准 I/O 和 JSONL 只动态依赖 `libc.so.6`；SQLite 样例在额外安装 `libsqlite3-0` 后执行。用 `readelf -dW` 核对每个应用的依赖，在 24.04 本机运行不替代最低版本验收。
