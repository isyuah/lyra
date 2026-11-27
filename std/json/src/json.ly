extern fn lyraJsonParse(text: String): CPtr;
extern fn lyraJsonEncode(value: CPtr, pretty: Bool): CPtr;
extern fn lyraJsonKind(value: CPtr): Int;
extern fn lyraJsonSize(value: CPtr): Int;
extern fn lyraJsonNumber(value: CPtr): Int;
extern fn lyraJsonReal(value: CPtr): Float64;
extern fn lyraJsonIsFinite(value: Float64): Bool;
extern fn lyraJsonFloat32Text(value: Float32): String;
extern fn lyraJsonFloat64Text(value: Float64): String;
extern fn lyraJsonText(value: CPtr): String;
extern fn lyraJsonKey(value: CPtr, index: Int): String;
extern fn lyraJsonIndex(value: CPtr, index: Int): CPtr;
extern fn lyraJsonFieldAt(value: CPtr, index: Int): CPtr;
extern fn lyraJsonField(value: CPtr, key: String): CPtr;
extern fn lyraJsonQuote(text: String): String;
extern fn lyraJsonSimpleKey(key: String): Bool;
extern fn lyraJsonIntText(value: Int): String;

/// 只读 JSON 节点。缺失节点和显式 JSON `null` 是两种不同状态。
pub struct JsonValue(val handle: CPtr,
    /// 组合解码时是否拒绝未声明字段；子节点继承此设置。
    pub val rejectsUnknownFields: Bool
) {
    /// 判断访问的字段或数组项是否不存在。
    pub fn missing(): Bool { lyraJsonKind(handle) == 0 }
    /// 判断是否为显式的 `null`，缺失节点不算 `null`。
    pub fn isNull(): Bool { lyraJsonKind(handle) == 1 }
    /// 判断是否为 JSON 对象，而非数组或标量。
    pub fn isObject(): Bool { lyraJsonKind(handle) == 2 }
    /// 判断是否为数组。
    pub fn isArray(): Bool { lyraJsonKind(handle) == 3 }
    /// 对象成员数或数组长度，其他节点返回 `0`。
    pub fn size(): Int { lyraJsonSize(handle) }
    /// 按零基下标读取对象成员的键；用 `size()` 限定遍历范围。
    pub fn key(index: Int): String { lyraJsonKey(handle, index) }
    /// 返回同一节点的宽松视图；组合解码会忽略未知字段，仍检查已知字段。
    pub fn ignoringUnknownFields(): JsonValue { JsonValue(handle, false) }
    /// 按零基下标取数组项；非数组或越界时返回缺失节点。
    pub fn item(index: Int): JsonValue { JsonValue(lyraJsonIndex(handle, index), rejectsUnknownFields) }
    /// 按字面键取对象字段，不把点号解释为路径；找不到时返回缺失节点。
    pub fn field(name: String): JsonValue { JsonValue(lyraJsonField(handle, name), rejectsUnknownFields) }
    /// 按零基下标取对象成员的值，与 `key(index)` 对应；越界返回缺失节点。
    pub fn fieldAt(index: Int): JsonValue { JsonValue(lyraJsonFieldAt(handle, index), rejectsUnknownFields) }
    /// 序列化为 JSON 文本；`pretty` 启用缩进，缺失节点不能编码。
    pub fn encode(pretty: Bool = false): String {
        requirePresent(this)
        val encoded = lyraJsonEncode(handle, pretty)
        if lyraJsonKind(encoded) == 8 { throw Exception(lyraJsonText(encoded)) }
        lyraJsonText(encoded)
    }
    /// 读取 JSON 字符串，不把其他标量转成文本；类型错误包含 `path`。
    pub fn readString(path: String): Result<String,String> {
        if lyraJsonKind(handle) == 4 { Ok(lyraJsonText(handle)) } else { Err(path + ": expected String") }
    }
    /// 读取恰含一个 Unicode 标量的 JSON 字符串。
    pub fn readChar(path: String): Result<Char,String> {
        if lyraJsonKind(handle) == 4 {
            val text = lyraJsonText(handle)
            if text.byteSize <= 4 && text.size == 1 { return Ok(text[0]) }
        }
        Err(path + ": expected Char (a string containing one Unicode scalar)")
    }
    fn integer(path: String, name: String): Result<Int,String> {
        if lyraJsonKind(handle) == 5 { Ok(lyraJsonNumber(handle)) } else { Err(path + ": expected " + name) }
    }
    /// 读取 JSON 整数，不接受浮点表示或数字字符串。
    pub fn readInt(path: String): Result<Int,String> { integer(path, "Int") }
    /// 读取有符号 64 位 JSON 整数，在类型错误中使用 `Int64` 名称。
    pub fn readInt64(path: String): Result<Int64,String> { integer(path, "Int64") }
    /// 读取整数并检查 `Int8` 的有符号范围。
    pub fn readInt8(path: String): Result<Int8,String> {
        match (integer(path, "Int8")?).toInt8Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for Int8") }
    }
    /// 读取整数并检查 `Int16` 的有符号范围。
    pub fn readInt16(path: String): Result<Int16,String> {
        match (integer(path, "Int16")?).toInt16Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for Int16") }
    }
    /// 读取整数并检查 `Int32` 的有符号范围。
    pub fn readInt32(path: String): Result<Int32,String> {
        match (integer(path, "Int32")?).toInt32Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for Int32") }
    }
    /// 读取 `0` 到 `255` 的 JSON 整数，拒绝负数和溢出。
    pub fn readUInt8(path: String): Result<UInt8,String> {
        match (integer(path, "UInt8")?).toUInt8Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt8") }
    }
    /// 读取整数并检查 `UInt16` 的非负范围。
    pub fn readUInt16(path: String): Result<UInt16,String> {
        match (integer(path, "UInt16")?).toUInt16Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt16") }
    }
    /// 读取整数并检查 `UInt32` 的非负范围。
    pub fn readUInt32(path: String): Result<UInt32,String> {
        match (integer(path, "UInt32")?).toUInt32Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt32") }
    }
    /// 将 JSON 整数或小数转成 `Float32`；舍入为正常浮点转换，溢出到无穷大时报错。
    pub fn readFloat32(path: String): Result<Float32,String> {
        val kind = lyraJsonKind(handle)
        if kind == 5 { return Ok(lyraJsonNumber(handle).toFloat32()) }
        if kind != 7 { return Err(path + ": expected Float32") }
        val narrowed = lyraJsonReal(handle).toFloat32()
        if lyraJsonIsFinite(narrowed) { Ok(narrowed) } else { Err(path + ": out of range for Float32") }
    }
    /// 读取 JSON 小数或将整数转换成 `Float64`；整数可能因浮点精度而舍入。
    pub fn readFloat64(path: String): Result<Float64,String> {
        val kind = lyraJsonKind(handle)
        if kind == 5 { return Ok(lyraJsonNumber(handle).toFloat64()) }
        if kind == 7 { Ok(lyraJsonReal(handle)) } else { Err(path + ": expected Float64") }
    }
    /// 按 `Float64` 规则读取 JSON 数值，使用默认浮点类型 `Float`。
    pub fn readFloat(path: String): Result<Float,String> { readFloat64(path) }
    /// 读取 JSON `true` 或 `false`；不接受数字或字符串替代。
    pub fn readBool(path: String): Result<Bool,String> {
        if lyraJsonKind(handle) == 6 { Ok(lyraJsonNumber(handle) != 0) } else { Err(path + ": expected Bool") }
    }
}

/// 解析完整 JSON 文本，包括标量根节点；成功时默认启用严格的未知字段策略。
pub fn parseJson(text: String): Result<JsonValue,String> {
    val value = lyraJsonParse(text)
    if lyraJsonKind(value) == 8 { Err(lyraJsonText(value)) } else { Ok(JsonValue(value, true)) }
}
/// 转义正文并加上双引号，得到可直接嵌入 JSON 的字符串字面量。
pub fn quoteJson(text: String): String { lyraJsonQuote(text) }
fn checkJsonKey(key: String): Unit {
    for byte in key.toBytes() {
        if byte == 0 { throw Exception("JSON object keys cannot contain NUL: " + quoteJson(key)) }
    }
}
fn requirePresent(value: JsonValue): Unit {
    if value.missing() { throw Exception("JSON cannot encode a missing value; use jsonNull() for null") }
}
/// 生成诊断路径的一段；普通键用 `.key`，其他键用带引号的方括号。
pub fn keyPath(key: String): String { if lyraJsonSimpleKey(key) { "." + key } else { "[" + quoteJson(key) + "]" } }
/// 将 `Int` 格式化为十进制 JSON 数字字面量。
pub fn intText(value: Int): String { lyraJsonIntText(value) }
/// 将有限 `Float32` 格式化为 JSON 数字；NaN 和无穷大抛异常。
pub fn float32Text(value: Float32): String {
    if !lyraJsonIsFinite(value) { throw Exception("JSON cannot encode a non-finite Float32") }
    lyraJsonFloat32Text(value)
}
/// 将有限 `Float64` 格式化为 JSON 数字；NaN 和无穷大抛异常。
pub fn float64Text(value: Float64): String {
    if !lyraJsonIsFinite(value) { throw Exception("JSON cannot encode a non-finite Float64") }
    lyraJsonFloat64Text(value)
}
/// 生成不带引号的 JSON 布尔字面量 `true` 或 `false`。
pub fn boolText(value: Bool): String { if value { "true" } else { "false" } }

/// 要求节点是对象；严格模式下报告首个不在 `keys` 内的字段，不检查必需字段是否存在。
pub fn checkFields(value: JsonValue, keys: List<String>, path: String): Result<Unit,String> {
    if !value.isObject() { return Err(path + ": expected object") }
    if value.rejectsUnknownFields {
        for index in 0..<value.size() {
            val key = value.key(index)
            var known = false
            for allowed in keys { if allowed == key { known = true; break } }
            if !known { return Err(path + keyPath(key) + ": unknown field") }
        }
    }
    Ok({})
}

/// 读取带标签对象的字符串 `tag`，错误包含字段路径。
pub fn readTag(value: JsonValue, path: String): Result<String,String> {
    if !value.isObject() { return Err(path + ": expected object") }
    value.field("tag").readString(path + ".tag")
}

/// 读取带标签对象的 `value`；可显式为 `null`，不能缺失。严格模式下仅允许 `tag` 和 `value`。
pub fn readTaggedValue(value: JsonValue, path: String): Result<JsonValue,String> {
    checkFields(value, ["tag", "value"], path)?
    val item = value.field("value")
    if item.missing() { return Err(path + ".value: missing field") }
    Ok(item)
}
