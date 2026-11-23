# Lyra

Lyra 是一门使用 LLVM AOT 的编译型语言。编译器把通过静态检查的源码编译成 Linux 可执行文件，不需要在部署机安装 LLVM。语言设计和验收契约见 [RFC 索引](rfc/README.md)，当前可以运行的语法见下文。

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

`check .../fail` 预期非零退出，在 `;` 处报告 `LYT0001`；最后一条命令应打印 `3` 并正常退出。要构建优化版编译器，把三个 preset 命令中的 `dev` 换成 `release`。用 `cmake --install build/dev --prefix <目录>` 安装工具链；`bin/lyra` 和 `lib/liblyraRuntime.a` 保持相对位置。部署**编译好的应用**只需复制产物到约定的 Linux 环境，不用带这两个文件、Clang 或 LLVM。

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
- `check` 不调用 Clang、不创建产物。`build` 默认输出 `<包目录>/build/debug/app`（`-O0`），`--release` 输出 `build/release/app`（`-O2`），`-o` 指定其他路径。类型错误或链接失败都不会覆盖已有产物。
- 诊断写到标准错误；`--diagnostics=json` 输出诊断数组，成功为 `[]`。带源码位置时，`range.start/end` 是左闭右开区间：`offset` 为从 0 开始的原文件 UTF-8 字节偏移，`line/column` 从 1 开始，列按 Unicode 码点计，CRLF 和 LF 均算一次换行。

## 验证 Ubuntu 22.04 部署

在仓库根目录运行 `docker build --platform linux/amd64 -f tests/jammy.Dockerfile --target verify .`。构建阶段在 Ubuntu 22.04 上编译工具链、运行 CTest、生成 Debug 与 Release 产物；运行阶段仅把这两个产物复制进新的 `ubuntu:22.04` 并执行。验收还通过 `readelf -dW` 检查每个程序只需要 `libc.so.6`。在 24.04 本机运行样例属于开发冒烟测试，不替代这一步。
