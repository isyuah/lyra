# 原生标准包和 C 边界

直接 `import std.io.readLine`、`std.sqlite.open` 或 `std.toml.parseToml`。编译器从随工具链安装的源码解析标准包，编译出的应用不需要复制这些 `.ly` 文件。

| 场景 | 标准库入口 |
|---|---|
| 文本、排序、去重 | [`std.text`](text/README.md) 的 `StringOps`；[`std.collections`](collections/README.md) 的 `ListOps`、`sortedBy`、`distinct`、`HashMap` |
| 标准流、文件和目录 | [`std.io`](io/README.md) 的 `LineReader`、`File`；[`std.fs`](fs/README.md) 的 `pathJoin`、`listDir`、`mkdirAll`、`atomicWrite` |
| 程序参数、环境与子进程 | [`std.os`](os/README.md) 的 `args`、`env`、`runProcess`；入口函数可用 `Int` 返回退出状态 |
| 选项与部署配置 | [`std.cli`](cli/README.md) 的 `Command`、`parseArgs`；[`std.config`](config/README.md) 的 `Config`、类型化配置键 |
| 日期、延时与日志 | [`std.time`](time/README.md) 的 `Clock`、`Date`、`DateTime`；内置 `await sleepMs(...)`；[`std.log`](log/README.md) 的 `Logger` |
| JSON、JSONL 和 TOML | [`std.json`](json/README.md) 的构造器和 codec；[`std.jsonl`](jsonl/README.md) 的 `jsonlReader`；[`std.toml`](toml/README.md) 的 TOML 值和 codec |
| SQLite 查询与写入 | [`std.sqlite`](sqlite/README.md) 的预编译语句、事务、`DbError` |
| HTTP 与 TLS | [`std.http`](http/README.md) 的 `Router` 和 `listen`；[`std.httpTls`](httpTls/README.md) 的 `listenTls`；[`std.httpClient`](httpClient/README.md) 的 `httpRequest` 和 `httpGet` |
| 测试断言 | 内置 `#assert(condition, message)`，失败位置指向调用处 |

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

声明 C 函数属于受信任的底层边界，作者必须对照 C 头文件保证 ABI、线程和生命周期相符。用 SDK 的 `<lyra/string.h>` 读取 `String`、`Bytes` 的只读视图；视图只在这次 C 调用内借用。普通 class 或内联 struct 不能直接出入这个 ABI，同一 C 符号也不能声明成两份不相容的签名。绑定需要原生指针或出参时，由薄 C 适配层明确转换；SQLite 包就是一个实例。可用 `lyra import-c` 显式生成头文件绑定；普通构建不自动查找第三方库，也不让 Lyra 异常越过 C 帧。
