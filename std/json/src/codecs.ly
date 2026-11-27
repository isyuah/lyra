import std.collections.{HashMap, stringMap}

/// 可组合的静态 JSON 映射，用于在 Lyra 值与 JSON 文本或节点之间转换。
pub struct JsonCodec<T>(val encodeValue: (T) -> String, val readValue: (JsonValue, String) -> Result<T,String>) {
    /// 通过当前映射编码一个完整 JSON 值。
    pub fn encode(value: T): String { encodeValue(value) }
    /// 解码一个已解析节点，使用 `path` 标记诊断位置，并保留节点的未知字段策略。
    pub fn read(value: JsonValue, path: String): Result<T,String> { readValue(value, path) }

    /// 解析完整 JSON 并从根路径 `$` 解码；默认拒绝未知字段。
    pub fn decode(text: String): Result<T,String> { read(parseJson(text)?, "$") }
    /// 解析完整 JSON，组合解码时忽略未知字段，已知字段仍照常检查。
    pub fn decodeIgnoringUnknownFields(text: String): Result<T,String> {
        read((parseJson(text)?).ignoringUnknownFields(), "$")
    }
}

/// 从自定义编码和读取闭包构造映射；编码闭包应返回一个合法 JSON 值，读取闭包应沿用给定诊断路径。
pub fn jsonCodec<T>(encode: (T) -> String, read: (JsonValue, String) -> Result<T,String>): JsonCodec<T> {
    JsonCodec(encode, read)
}

/// JSON 字符串映射，自动转义，不接受其他节点种类。
pub fn jsonCodecString(): JsonCodec<String> { jsonCodec<String>(quoteJson, (value, path) => { value.readString(path) }) }
/// 把 `Char` 编码为恰含一个 Unicode 标量的 JSON 字符串。
pub fn jsonCodecChar(): JsonCodec<Char> { jsonCodec<Char>((value) => { quoteJson(value.toString()) }, (value, path) => { value.readChar(path) }) }
/// 映射有符号 64 位 JSON 整数，不接受小数或字符串。
pub fn jsonCodecInt(): JsonCodec<Int> { jsonCodec<Int>(intText, (value, path) => { value.readInt(path) }) }
/// 同 `Int` 的整数表示，在解码错误中使用 `Int64` 名称。
pub fn jsonCodecInt64(): JsonCodec<Int64> { jsonCodec<Int64>(intText, (value, path) => { value.readInt64(path) }) }
/// 编码为 JSON 整数；解码检查 `Int8` 范围。
pub fn jsonCodecInt8(): JsonCodec<Int8> { jsonCodec<Int8>((value) => { intText(value) }, (node, path) => { node.readInt8(path) }) }
/// 编码为 JSON 整数；解码检查 `Int16` 范围。
pub fn jsonCodecInt16(): JsonCodec<Int16> { jsonCodec<Int16>((value) => { intText(value) }, (node, path) => { node.readInt16(path) }) }
/// 编码为 JSON 整数；解码检查 `Int32` 范围。
pub fn jsonCodecInt32(): JsonCodec<Int32> { jsonCodec<Int32>((value) => { intText(value) }, (node, path) => { node.readInt32(path) }) }
/// 编码为非负 JSON 整数；解码检查 `UInt8` 范围。
pub fn jsonCodecUInt8(): JsonCodec<UInt8> { jsonCodec<UInt8>((value) => { intText(value) }, (node, path) => { node.readUInt8(path) }) }
/// 编码为非负 JSON 整数；解码检查 `UInt16` 范围。
pub fn jsonCodecUInt16(): JsonCodec<UInt16> { jsonCodec<UInt16>((value) => { intText(value) }, (node, path) => { node.readUInt16(path) }) }
/// 编码为非负 JSON 整数；解码检查 `UInt32` 范围。
pub fn jsonCodecUInt32(): JsonCodec<UInt32> { jsonCodec<UInt32>((value) => { intText(value) }, (node, path) => { node.readUInt32(path) }) }
/// 映射 JSON 原生布尔值，不做数字或字符串转换。
pub fn jsonCodecBool(): JsonCodec<Bool> { jsonCodec<Bool>(boolText, (value, path) => { value.readBool(path) }) }
/// 映射有限单精度浮点值；读取整数或小数，检查是否溢出为无穷大。
pub fn jsonCodecFloat32(): JsonCodec<Float32> { jsonCodec<Float32>(float32Text, (value, path) => { value.readFloat32(path) }) }
/// 映射有限双精度浮点值；读取整数或小数，编码拒绝 NaN 和无穷大。
pub fn jsonCodecFloat64(): JsonCodec<Float64> { jsonCodec<Float64>(float64Text, (value, path) => { value.readFloat64(path) }) }
/// 把 `Unit` 映射为 JSON `null`；缺失节点和其他值都不是 `Unit`。
pub fn jsonCodecUnit(): JsonCodec<Unit> {
    jsonCodec<Unit>((_) => { "null" }, (value, path) => { if value.isNull() { Ok({}) } else { Err(path + ": expected null") } })
}

/// 用显式 `null` 表示 `None`，非空节点交给元素映射；缺失不是 `null`。
pub fn jsonCodecNullable<T>(element: JsonCodec<T>): JsonCodec<Option<T>> {
    jsonCodec<Option<T>>((value) => { match value { None => "null", Some(item) => element.encode(item) } },
                         (node, path) => { if node.isNull() { Ok(None) } else { Ok(Some(element.read(node, path)?)) } })
}

/// 将缺失字段解码成 `None`，有值的字段（含 `null`）交给元素映射。直接编码 `None` 会输出 `null`，不会删除字段。
pub fn jsonCodecOptionalField<T>(element: JsonCodec<T>): JsonCodec<Option<T>> {
    jsonCodec<Option<T>>((value) => { match value { None => "null", Some(item) => element.encode(item) } },
                         (node, path) => { if node.missing() { Ok(None) } else { Ok(Some(element.read(node, path)?)) } })
}

/// 按顺序把列表映射为 JSON 数组；元素错误路径包含零基数组下标。
pub fn jsonCodecList<T>(element: JsonCodec<T>): JsonCodec<List<T>> {
    jsonCodec<List<T>>((items) => {
        var text = "["
        for index in 0..<items.size {
            if index != 0 { text += "," }
            text += element.encode(items[index])
        }
        text + "]"
    }, (node, path) => {
        if !node.isArray() { return Err(path + ": expected array") }
        val items: List<T> = []
        for index in 0..<node.size() { items.add(element.read(node.item(index), path + "[" + intText(index) + "]")?) }
        Ok(items)
    })
}

/// 以 `tag` 区分 `None` 和 `Some`；`Some` 用 `value` 保存载荷，可保留多层可选值的状态。
pub fn jsonCodecOption<T>(element: JsonCodec<T>): JsonCodec<Option<T>> {
    jsonCodec<Option<T>>((value) => {
        match value { None => r#"{"tag":"None"}"#, Some(item) => r#"{"tag":"Some","value":"# + element.encode(item) + "}" }
    }, (node, path) => {
        val tag = readTag(node, path)?
        match tag {
            "None" => { checkFields(node, ["tag"], path)?; Ok(None) },
            "Some" => Ok(Some(element.read(readTaggedValue(node, path)?, path + ".value")?)),
            _ => Err(path + ".tag: unknown Option tag " + quoteJson(tag)),
        }
    })
}

/// 用对象的 `tag` 标记 `Ok` 或 `Err`，用各自的映射读写 `value` 载荷。
pub fn jsonCodecResult<T,E>(success: JsonCodec<T>, failure: JsonCodec<E>): JsonCodec<Result<T,E>> {
    jsonCodec<Result<T,E>>((value) => {
        match value {
            Ok(item) => r#"{"tag":"Ok","value":"# + success.encode(item) + "}",
            Err(error) => r#"{"tag":"Err","value":"# + failure.encode(error) + "}",
        }
    }, (node, path) => {
        val tag = readTag(node, path)?
        val itemPath = path + ".value"
        match tag {
            "Ok" => Ok(Ok(success.read(readTaggedValue(node, path)?, itemPath)?)),
            "Err" => Ok(Err(failure.read(readTaggedValue(node, path)?, itemPath)?)),
            _ => Err(path + ".tag: unknown Result tag " + quoteJson(tag)),
        }
    })
}

/// 把字符串键映射为 JSON 对象的字面键；字段值使用元素映射，不保证输出字段顺序。
pub fn jsonCodecStringMap<T>(element: JsonCodec<T>): JsonCodec<HashMap<String,T>> {
    jsonCodec<HashMap<String,T>>((map) => {
        var text = "{"
        for entry in map.entries() {
            checkJsonKey(entry.key)
            if text != "{" { text += "," }
            text += quoteJson(entry.key) + ":" + element.encode(entry.value)
        }
        text + "}"
    }, (node, path) => {
        if !node.isObject() { return Err(path + ": expected object") }
        val map = stringMap<T>()
        for index in 0..<node.size() {
            val key = node.key(index)
            map.put(key, element.read(node.fieldAt(index), path + keyPath(key))?);
        }
        Ok(map)
    })
}
