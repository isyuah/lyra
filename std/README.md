# 原生标准包和 C 边界

直接 `import std.io.readLine` 或 `import std.sqlite.open`。编译器从随工具链安装的源码解析标准包，编译出的应用不需要复制这些 `.ly` 文件。[文本输入输出](io/README.md)和[SQLite](sqlite/README.md)分别列了真实用法与错误契约。

在低层绑定中可以按原 C 符号显式声明 `extern fn`。例如，Linux x86-64 的 libc `llabs` 使用 64 位整数：

```text
extern fn llabs(value: Int): Int;
fn main(): Unit { print(llabs(-12)) }
```

| Lyra 声明 | 本切片的 C ABI |
|---|---|
| `Int` | `int64_t`，不是常见的 32 位 C `int` |
| `Bool` | `uint8_t`，传 `0/1`，返回非零为真 |
| `CPtr` | 不透明的 C 指针；安全 Lyra 不解引用、不做指针算术 |
| `String` | Lyra 不可变、有长度的 UTF-8 字符串句柄，**不是 `char*`** |
| 返回 `Unit` | C `void`；不能作为形参 |

声明 C 函数属于受信任的底层边界，作者必须对照 C 头文件保证 ABI、线程和生命周期相符。普通 class 或内联 struct 不能直接出入这个 ABI，同一 C 符号也不能声明成两份不相容的签名。绑定需要原生 `char*`、出参、32 位值时，由薄 C 适配层明确转换；SQLite 包就是一个实例。不会自动寻找任意第三方库、解析 C 头文件或让 Lyra 异常越过 C 帧。
