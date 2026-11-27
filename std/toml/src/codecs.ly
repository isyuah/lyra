import std.collections.{HashMap, stringMap}

/// 可组合的静态 TOML 映射，读写时保留嵌套位置的诊断路径。
pub struct TomlCodec<T>(val writeValue: (T, String, Bool) -> String, val readValue: (TomlValue, String) -> Result<T,String>) {
    /// 编码一个值；`inline` 为真时返回能嵌入外层文档的片段。
    pub fn write(value: T, path: String, inline: Bool): String { writeValue(value, path, inline) }
    /// 从已解析节点解码，沿用节点的未知键策略。
    pub fn read(value: TomlValue, path: String): Result<T,String> { readValue(value, path) }
}

/// 用自定义读写闭包构造映射；编码应遵守内联模式，读写应沿用给定诊断路径。
pub fn tomlCodec<T>(write: (T, String, Bool) -> String, read: (TomlValue, String) -> Result<T,String>): TomlCodec<T> {
    TomlCodec(write, read)
}

/// 映射 TOML 字符串，编码自动加引号和转义。
pub fn tomlCodecString(): TomlCodec<String> { tomlCodec<String>((value, path, inline) => { quoteToml(value) }, (node, path) => { node.readString(path) }) }
/// 映射 TOML 布尔字面量 `true` 与 `false`，不接受字符串或整数。
pub fn tomlCodecBool(): TomlCodec<Bool> { tomlCodec<Bool>((value, path, inline) => { value.toString() }, (node, path) => { node.readBool(path) }) }
/// 映射有符号 64 位 TOML 整数，整数与浮点节点分开处理。
pub fn tomlCodecInt(): TomlCodec<Int> { tomlCodec<Int>((value, path, inline) => { value.toString() }, (node, path) => { node.readInt(path) }) }
/// 写出 TOML 整数，读取时检查 `Int8` 范围。
pub fn tomlCodecInt8(): TomlCodec<Int8> { tomlCodec<Int8>((value, path, inline) => { value.toString() }, (node, path) => { node.readInt8(path) }) }
/// 写出 TOML 整数，读取时检查 `Int16` 范围。
pub fn tomlCodecInt16(): TomlCodec<Int16> { tomlCodec<Int16>((value, path, inline) => { value.toString() }, (node, path) => { node.readInt16(path) }) }
/// 写出 TOML 整数，读取时检查 `Int32` 范围。
pub fn tomlCodecInt32(): TomlCodec<Int32> { tomlCodec<Int32>((value, path, inline) => { value.toString() }, (node, path) => { node.readInt32(path) }) }
/// 写出非负 TOML 整数，读取时检查 `UInt8` 范围。
pub fn tomlCodecUInt8(): TomlCodec<UInt8> { tomlCodec<UInt8>((value, path, inline) => { value.toString() }, (node, path) => { node.readUInt8(path) }) }
/// 写出非负 TOML 整数，读取时检查 `UInt16` 范围。
pub fn tomlCodecUInt16(): TomlCodec<UInt16> { tomlCodec<UInt16>((value, path, inline) => { value.toString() }, (node, path) => { node.readUInt16(path) }) }
/// 写出非负 TOML 整数，读取时检查 `UInt32` 范围。
pub fn tomlCodecUInt32(): TomlCodec<UInt32> { tomlCodec<UInt32>((value, path, inline) => { value.toString() }, (node, path) => { node.readUInt32(path) }) }
/// 映射 `UInt64` 与非负 TOML 整数；编码超过 `Int64` 最大值时抛异常。
pub fn tomlCodecUInt64(): TomlCodec<UInt64> { tomlCodec<UInt64>((value, path, inline) => { uint64Text(value, path) }, (node, path) => { node.readUInt64(path) }) }
/// 映射单精度 TOML 浮点；有限值缩窄成无穷大时报错。
pub fn tomlCodecFloat32(): TomlCodec<Float32> { tomlCodec<Float32>((value, path, inline) => { float32Text(value) }, (node, path) => { node.readFloat32(path) }) }
/// 映射双精度 TOML 浮点，包括 TOML 的无穷大、NaN 和有符号零。
pub fn tomlCodecFloat64(): TomlCodec<Float64> { tomlCodec<Float64>((value, path, inline) => { float64Text(value) }, (node, path) => { node.readFloat64(path) }) }

/// 把列表映射为数组；每个元素都用可内联的表示，诊断路径附加数组下标。
pub fn tomlCodecList<T>(element: TomlCodec<T>): TomlCodec<List<T>> {
    tomlCodec<List<T>>((items, path, _) => { writeArray<T>(items, path, (item, at) => { element.write(item, at, true) }) },
                       (node, path) => { readArray<T>(node, path, (item, at) => { element.read(item, at) }) })
}

/// 将缺键解码为 `None`，存在的键交给元素映射。TOML 没有空字面量，单独编码 `None` 会抛异常。
pub fn tomlCodecOptionalField<T>(element: TomlCodec<T>): TomlCodec<Option<T>> {
    tomlCodec<Option<T>>((value, path, inline) => {
        match value {
            None => throw Exception(path + ": cannot omit None with a custom TOML default"),
            Some(item) => element.write(item, path, inline),
        }
    }, (node, path) => { if node.missing() { Ok(None) } else { Ok(Some(element.read(node, path)?)) } })
}

/// 写出 `tag` 和可选的 `value`；`payload` 是已经编码好的 TOML 值片段。
pub fn writeTagged(tag: String, payload: String?, inline: Bool): String {
    val entries = ["tag = " + quoteToml(tag)]
    if let Some(text) = payload { entries.add("value = " + text); }
    tableText(entries, inline)
}

/// 从表中读取字符串 `tag`，缺失或类型错误包含字段路径。
pub fn readTag(value: TomlValue, path: String): Result<String,String> {
    if !value.isTable() { return Err(path + ": expected table") }
    value.field("tag").readString(path + ".tag")
}

/// 取必需的 `value` 载荷；严格模式下表只允许 `tag` 与 `value`。
pub fn readTaggedValue(value: TomlValue, path: String): Result<TomlValue,String> {
    checkFields(value, ["tag", "value"], path)?
    val item = value.field("value")
    if item.missing() { return Err(path + ".value: missing field") }
    Ok(item)
}

/// 将可选值表示为带 `None` 或 `Some` 标签的表；`Some` 通过 `value` 存储载荷。
pub fn tomlCodecOption<T>(element: TomlCodec<T>): TomlCodec<Option<T>> {
    tomlCodec<Option<T>>((value, path, inline) => {
        match value {
            None => writeTagged("None", None, inline),
            Some(item) => writeTagged("Some", Some(element.write(item, path + ".value", true)), inline),
        }
    }, (node, path) => {
        val tag = readTag(node, path)?
        match tag {
            "None" => { checkFields(node, ["tag"], path)?; Ok(None) },
            "Some" => Ok(Some(element.read(readTaggedValue(node, path)?, path + ".value")?)),
            _ => Err(path + ".tag: unknown Option tag " + quoteToml(tag)),
        }
    })
}

/// 将结果映射为带 `Ok` 或 `Err` 标签的表，并用对应 codec 读写载荷。
pub fn tomlCodecResult<T,E>(success: TomlCodec<T>, failure: TomlCodec<E>): TomlCodec<Result<T,E>> {
    tomlCodec<Result<T,E>>((value, path, inline) => {
        match value {
            Ok(item) => writeTagged("Ok", Some(success.write(item, path + ".value", true)), inline),
            Err(error) => writeTagged("Err", Some(failure.write(error, path + ".value", true)), inline),
        }
    }, (node, path) => {
        val tag = readTag(node, path)?
        match tag {
            "Ok" => Ok(Ok(success.read(readTaggedValue(node, path)?, path + ".value")?)),
            "Err" => Ok(Err(failure.read(readTaggedValue(node, path)?, path + ".value")?)),
            _ => Err(path + ".tag: unknown Result tag " + quoteToml(tag)),
        }
    })
}

/// 把字符串键映射为 TOML 表的字面键；值使用元素 codec，写出顺序不固定。
pub fn tomlCodecStringMap<T>(element: TomlCodec<T>): TomlCodec<HashMap<String,T>> {
    tomlCodec<HashMap<String,T>>((map, path, inline) => {
        val entries: List<String> = []
        for entry in map.entries() {
            entries.add(tableKey(entry.key) + " = " + element.write(entry.value, fieldPath(path, entry.key), true));
        }
        tableText(entries, inline)
    }, (node, path) => {
        if !node.isTable() { return Err(path + ": expected table") }
        val map = stringMap<T>()
        for index in 0..<node.size() {
            val key = node.key(index)
            map.put(key, element.read(node.field(key), fieldPath(path, key))?);
        }
        Ok(map)
    })
}
