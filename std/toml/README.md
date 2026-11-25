# `std.toml`

`#derive(Toml)` 为当前模块中的 `struct` 和 `enum` 生成配置文档的读写函数。`decodeConfigToml(text)` 返回 `Result<Config,String>`，`encodeConfigToml(config)` 返回可再次解析的 TOML 文本。

```lyra
#derive(Toml)
struct Server(pub val host: String, pub val port: UInt16 = 8080)

#derive(Toml)
struct Config(pub val servers: List<Server>, pub val debug: Bool = false)
```

这个配置可以直接读取 TOML 的数组表。

```toml
debug = true

[[servers]]
host = "localhost"

[[servers]]
host = "example.org"
port = 443
```

第一个服务使用默认端口 `8080`。第二个服务的 `port` 若写为 `100000`，解码器返回 `$.servers[1].port: out of range for UInt16`。写出配置时，顶层按字段生成键值对，嵌套 Struct 使用内联表，列表使用 TOML 数组。往返保留字段值，重新编码使用统一的空白和引号格式。

## 字段映射

- `Bool`、`String`、定宽整数、`Int`、`UInt`、`Float32`、`Float64`、`Float` 以及由它们组成的列表与嵌套 Struct 都可以派生。嵌套 Struct 需要在当前模块提供自己的 TOML 映射。
- TOML 的整数范围是有符号 64 位。窄类型检查取值范围，`UInt` / `UInt64` 超过 `Int64` 最大值时，编码器抛出包含字段路径的 `Exception`。整数字段接受 TOML 整数，浮点字段接受 TOML 浮点，例如 `rate = 1.0`。浮点支持 `inf`、`nan` 和有符号零。
- `[tomlName("app.port")]` 使用字面键 `"app.port"`，其错误路径为 `$["app.port"]`。字段别名须非空、互不重复且不含 NUL。默认严格处理未知键。`decodeConfigTomlIgnoringUnknownFields(text)` 在整个嵌套文档中跳过未知键，已知键照常检查类型。
- 缺键采用构造默认值，按字段声明顺序求值，可以读取已确定的前置字段。只映射公开字段，私有字段通过声明处的显式默认值初始化。
- 可空字段在无默认值或默认值直接写 `None` / `null` 时，以缺键表示 `None`。若有其他默认表达式，缺键用于触发该表达式，此时编码 `None` 会抛出带字段路径的 `Exception`。TOML 没有空值，`Option<Option<T>>` 和 `List<T?>` 需要自定义表示。
- `parseToml(text)` 返回 `Result<TomlValue,String>`，可直接读取表、数组和标量。`field(key)` 选择字面键，`item(index)` 选择数组元素，`missing()` 检查缺失。日期和时间可以解析并通过 `isTemporal()` 识别，首版保留为时间节点，业务类型的时间映射留待日期标准库。

`TomlCodec<T>` 可跨包复用：`read(node, path)` 继承已有解析节点的未知键策略，`write(value, path, inline)` 的 `inline` 为 `true` 时生成可嵌入文档的值。泛型派生工厂按声明顺序接收各类型实参的 codec，例如 `tomlCodecConfig(tomlCodecString())`。用 `[tomlCodec("wire")]` 指定字段最内层类型的手写或外部 codec 工厂。

Enum 编码为包含 `tag` 的表；单位分支只写 `tag`，带一个载荷写 `value`，多个载荷依序写入 `value` 数组。`tomlCodecList`、`tomlCodecStringMap`、`tomlCodecResult` 和 `tomlCodecOption` 可以组合；显式 Option 使用 `None` / `Some` 标签，可保留嵌套空值状态。普通可空字段仍沿用缺键规则。

## 构建

运行时静态编入 MIT 许可的 tomlc17 R251129。首次构建用 CMake 获取已锁定摘要的源码。离线构建可传 `-DFETCHCONTENT_SOURCE_DIR_LYRA_TOMLC17=<解压的源码目录>`。解析入口每份文档接受最多 16 MiB。Windows 和 Linux 使用相同的 TOML 1.0 解析器。[tomlc17 对应版本](https://github.com/cktan/tomlc17/tree/f6c98d0f69993a18e75cb8cc6cad672f0942c107)
