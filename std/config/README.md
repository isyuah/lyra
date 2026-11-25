# `std.config`

```lyra
import std.cli.{ParsedCommand}
import std.config.{Config}
import std.config.{ConfigError}
import std.config.{intKey}

fn port(arguments: ParsedCommand): Result<Int,ConfigError> {
    val settings = Config.load("server.toml")?
    val key = intKey("server.port", 8080, cliName: Some("--port"), envName: Some("PORT"))
    settings.get(key, Some(arguments))
}
```

按 CLI、环境变量、TOML、默认值的顺序取第一个存在的值。CLI 与环境变量用键的文本解码器；TOML 用 `TomlCodec<T>`。常见标量用 `stringKey`、`intKey` 和 `boolKey`；自定义键用 `configKey(path, codec, parseText, defaultValue, cliName:, envName:)`。布尔键可绑定 CLI 开关；未传开关会继续查找其他来源。路径用 `.` 遍历多层表。

`Config.empty()` 不加载文件；`Config.load(path)` 和 `Config.fromToml(text)` 分别读取 UTF-8 文件与 TOML 文本。更高优先级的值存在但类型非法时，错误包含配置来源，不回退，也不回显值。显式空字符串视为已设置。
