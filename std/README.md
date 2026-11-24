# 原生标准包和 C 边界

直接 `import std.io.readLine`、`std.sqlite.open` 或 `std.toml.parseToml`。编译器从随工具链安装的源码解析标准包，编译出的应用不需要复制这些 `.ly` 文件。[文本和文件](io/README.md)、[SQLite](sqlite/README.md)和[TOML](toml/README.md)分别列了真实用法与错误契约。

JSON 与 TOML 都使用显式 `#derive(Json)` / `#derive(Toml)` 生成静态编解码实现；格式不能表示的多层空值、对象身份图等仍需显式映射，不会静默丢信息。

需要按运行时类型检查公开字段时，可显式导入 `std.reflect.typeName`、`fieldCount`、`fieldNames` 和 `field`。反射 API 不替代静态 JSON/TOML 派生，也不暴露私有字段。

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
| `Bytes` | Lyra 不可变、带长度的原始字节句柄，**不是 `unsigned char*`** |
| 返回 `Unit` | C `void`；不能作为形参 |

声明 C 函数属于受信任的底层边界，作者必须对照 C 头文件保证 ABI、线程和生命周期相符。用 SDK 的 `<lyra/string.h>` 读取 `String`、`Bytes` 的只读视图；视图只在这次 C 调用内借用。普通 class 或内联 struct 不能直接出入这个 ABI，同一 C 符号也不能声明成两份不相容的签名。绑定需要原生指针或出参时，由薄 C 适配层明确转换；SQLite 包就是一个实例。不会自动寻找任意第三方库、解析 C 头文件或让 Lyra 异常越过 C 帧。
