# Lyra

Lyra 是一门使用 LLVM AOT 的编译型语言。当前编译器跑通了[第一个正反例](rfc/compiler-acceptance.md)：同一个程序，有末尾分号时报告返回类型错误，去掉分号后编译成打印 `3` 的 Linux 可执行文件。完整语言设计见 [RFC 索引](rfc/README.md)；GC、异步、泛型、宏和包解析还不是这个编译器的功能。

## 从源码构建

在 **x86-64 Linux** 的仓库根目录执行；Windows 开发机先进入 WSL 2 的 Ubuntu 24.04。需要 CMake 3.20+、C++20 工具链、LLVM 18.1 开发包、`clang-18` 和 Python 3（测试）。安装步骤见[原生工具链 RFC](rfc/native-toolchain.md)。

```sh
cmake -S . -B build -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 -DLLVM_DIR="$(llvm-config-18 --cmakedir)"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
build/bin/lyra check examples/compiler-smoke/fail --diagnostics=json
build/bin/lyra build examples/compiler-smoke/pass --release
examples/compiler-smoke/pass/build/release/app
```

`check .../fail` 预期非零退出，在 `;` 处报告 `LYT0001`；最后一条命令应打印 `3` 并正常退出。也可以用 `cmake --install build --prefix <目录>` 安装编译器；`bin/lyra` 和 `lib/liblyraRuntime.a` 保持相对位置。部署**编译好的应用**只需复制产物到约定的 Linux 环境，不用带这两个文件、Clang 或 LLVM。

## 命令与语言子集

```text
lyra check <包目录> [--diagnostics=json]
lyra build <包目录> [-o <产物>] [--release] [--diagnostics=json]
```

- 两条命令只读取包内 `src` 直属的 `.ly` 文件，作为一个可执行模块，入口为 `fn main(): Unit`。不递归、不需要包清单；同模块函数可前向引用。
- 支持显式返回 `Int` / `Unit` 的无参函数、整数和检查溢出的加法、括号、嵌套函数调用、换行或分号分隔的表达式、`print(Int)`。块正常结束时取最后一个表达式；空块或带末尾分号的块取 `Unit`。`print` 输出十进制整数及换行。
- `check` 不调用 Clang、不创建产物。`build` 默认输出 `<包目录>/build/debug/app`（`-O0`），`--release` 输出 `build/release/app`（`-O2`），`-o` 指定其他路径。类型错误或链接失败都不会覆盖已有产物。
- 诊断写到标准错误；`--diagnostics=json` 输出诊断数组，成功为 `[]`。带源码位置时，`range.start/end` 是左闭右开区间：`offset` 为从 0 开始的原文件 UTF-8 字节偏移，`line/column` 从 1 开始，列按 Unicode 码点计，CRLF 和 LF 均算一次换行。

## 验证 Ubuntu 22.04 部署

在仓库根目录运行 `docker build --platform linux/amd64 -f tests/jammy.Dockerfile --target verify .`。构建阶段在 Ubuntu 22.04 上编译工具链、运行 CTest、生成 Debug 与 Release 产物；运行阶段仅把这两个产物复制进新的 `ubuntu:22.04` 并执行。验收还通过 `readelf -dW` 检查每个程序只需要 `libc.so.6`。在 24.04 本机运行样例属于开发冒烟测试，不替代这一步。
