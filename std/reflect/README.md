# std.reflect

显式运行时字段反射入口。`Any.typeName()` 返回可读类型名；对 Struct/Class，`fieldCount()` 与 `fieldNames()` 只列出公开字段，`field(name)` 返回 `Option<Any>`。枚举和其他类型当前没有可枚举字段。

字段读取保留字段值的原始运行时类型身份，可以继续通过 `as? T` 检查转换。反射只读取公开字段，不提供动态写入、构造或 Class 身份图遍历；JSON/TOML 仍默认使用静态 `#derive` 编解码。
