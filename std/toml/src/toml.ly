extern fn lyraTomlParse(source: String): CPtr;
extern fn lyraTomlKind(node: CPtr): Int;
extern fn lyraTomlSize(node: CPtr): Int;
extern fn lyraTomlText(node: CPtr): String;
extern fn lyraTomlInteger(node: CPtr): Int;
extern fn lyraTomlFloat(node: CPtr): Float64;
extern fn lyraTomlIsFinite(value: Float64): Bool;
extern fn lyraTomlField(node: CPtr, key: String): CPtr;
extern fn lyraTomlItem(node: CPtr, index: Int): CPtr;
extern fn lyraTomlKey(node: CPtr, index: Int): String;
extern fn lyraTomlQuote(value: String): String;
extern fn lyraTomlFloat32Text(value: Float32): String;
extern fn lyraTomlFloat64Text(value: Float64): String;

/// 只读 TOML 节点，可检查表、数组、标量或缺失状态。
pub struct TomlValue(val handle: CPtr,
    /// 组合解码时是否拒绝未知键；子节点沿用这个设置。
    pub val rejectsUnknownFields: Bool
) {
    /// 判断访问的键或数组项是否不存在；TOML 没有显式 `null`。
    pub fn missing(): Bool { lyraTomlKind(handle) == 0 }
    /// 判断是否为表，包括内联表。
    pub fn isTable(): Bool { lyraTomlKind(handle) == 1 }
    /// 判断是否为数组，包括数组表。
    pub fn isArray(): Bool { lyraTomlKind(handle) == 2 }
    /// 判断是否为 TOML 日期、时间或日期时间节点。
    pub fn isTemporal(): Bool { lyraTomlKind(handle) == 7 }
    /// 表的键数或数组长度；其他节点返回 `0`。
    pub fn size(): Int { lyraTomlSize(handle) }
    /// 按零基下标读取表的键；用 `size()` 限定遍历范围。
    pub fn key(index: Int): String { lyraTomlKey(handle, index) }
    /// 按字面键取表字段；点号不表示层级，找不到时返回缺失节点。
    pub fn field(key: String): TomlValue { TomlValue(lyraTomlField(handle, key), rejectsUnknownFields) }
    /// 按零基下标取数组项；非数组或越界时返回缺失节点。
    pub fn item(index: Int): TomlValue { TomlValue(lyraTomlItem(handle, index), rejectsUnknownFields) }
    /// 返回同一节点的宽松视图，解码时跳过未知键，仍检查已知键的类型。
    pub fn ignoringUnknownFields(): TomlValue { TomlValue(handle, false) }

    /// 读取 TOML 字符串；不会把数字、布尔或时间节点转成文本。
    pub fn readString(path: String): Result<String,String> {
        if lyraTomlKind(handle) == 3 { Ok(lyraTomlText(handle)) } else { Err(path + ": expected String") }
    }
    /// 读取 TOML 原生布尔值，不接受字符串替代。
    pub fn readBool(path: String): Result<Bool,String> {
        if lyraTomlKind(handle) == 5 { Ok(lyraTomlInteger(handle) != 0) } else { Err(path + ": expected Bool") }
    }
    /// 读取有符号 64 位 TOML 整数，拒绝浮点节点。
    pub fn readInt(path: String): Result<Int,String> {
        if lyraTomlKind(handle) == 4 { Ok(lyraTomlInteger(handle)) } else { Err(path + ": expected Int") }
    }
    /// 读取整数并检查 `Int8` 的有符号范围。
    pub fn readInt8(path: String): Result<Int8,String> {
        match (readInt(path)?).toInt8Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for Int8") }
    }
    /// 读取整数并检查 `Int16` 的有符号范围。
    pub fn readInt16(path: String): Result<Int16,String> {
        match (readInt(path)?).toInt16Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for Int16") }
    }
    /// 读取整数并检查 `Int32` 的有符号范围。
    pub fn readInt32(path: String): Result<Int32,String> {
        match (readInt(path)?).toInt32Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for Int32") }
    }
    /// 读取 `0` 到 `255` 的 TOML 整数。
    pub fn readUInt8(path: String): Result<UInt8,String> {
        match (readInt(path)?).toUInt8Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt8") }
    }
    /// 读取整数并检查 `UInt16` 的非负范围。
    pub fn readUInt16(path: String): Result<UInt16,String> {
        match (readInt(path)?).toUInt16Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt16") }
    }
    /// 读取整数并检查 `UInt32` 的非负范围。
    pub fn readUInt32(path: String): Result<UInt32,String> {
        match (readInt(path)?).toUInt32Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt32") }
    }
    /// 将非负 TOML 整数读为 `UInt64`；输入仍受 TOML `Int64` 上限约束。
    pub fn readUInt64(path: String): Result<UInt64,String> {
        match (readInt(path)?).toUInt64Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt64") }
    }
    /// 读取 TOML 浮点节点（包括无穷大与 NaN），不把整数节点隐式转成浮点。
    pub fn readFloat64(path: String): Result<Float64,String> {
        if lyraTomlKind(handle) == 6 { Ok(lyraTomlFloat(handle)) } else { Err(path + ": expected Float64") }
    }
    /// 将 TOML 浮点缩窄为 `Float32`；有限数转成无穷大时返回范围错误。
    pub fn readFloat32(path: String): Result<Float32,String> {
        val number = readFloat64(path)?
        val narrowed = number.toFloat32()
        if lyraTomlIsFinite(number) && !lyraTomlIsFinite(narrowed) { return Err(path + ": out of range for Float32") }
        Ok(narrowed)
    }
}

/// 解析最多 `16 MiB` 的完整 TOML 1.0 文档，返回根表；解析错误为文本诊断。
pub fn parseToml(text: String): Result<TomlValue,String> {
    val handle = lyraTomlParse(text)
    if lyraTomlKind(handle) == 8 { Err(lyraTomlText(handle)) } else { Ok(TomlValue(handle, true)) }
}

/// 转义文本并加双引号，生成 TOML 基本字符串字面量。
pub fn quoteToml(value: String): String { lyraTomlQuote(value) }
/// 生成单精度 TOML 浮点字面量，保留浮点种类，支持 `inf` 与 `nan`。
pub fn float32Text(value: Float32): String { lyraTomlFloat32Text(value) }
/// 生成双精度 TOML 浮点字面量，保留浮点种类，支持 `inf` 与 `nan`。
pub fn float64Text(value: Float64): String { lyraTomlFloat64Text(value) }
/// 将无符号整数编码成 TOML 整数；大于 `Int64` 最大值时抛出带 `path` 的异常。
pub fn uint64Text(value: UInt64, path: String): String {
    if value.toInt64Checked() == None { throw Exception(path + ": TOML integer exceeds Int64") }
    value.toString()
}

fn simpleKey(key: String): Bool {
    val bytes = key.toBytes()
    if bytes.size == 0 { return false }
    for index in 0..<bytes.size {
        val byte = bytes[index]
        val letter = (byte >= 65 && byte <= 90) || (byte >= 97 && byte <= 122)
        if !(letter || byte == 95 || (index != 0 && byte >= 48 && byte <= 57)) { return false }
    }
    true
}

/// 编码单个字面表键；点号等字符会被引号保护，不产生多级路径。
pub fn tableKey(key: String): String { if simpleKey(key) { key } else { quoteToml(key) } }
/// 为错误位置追加一个表键，简单键用点号，其他键用引号和方括号。
pub fn fieldPath(path: String, key: String): String {
    if simpleKey(key) { path + "." + key } else { path + "[" + quoteToml(key) + "]" }
}

/// 拼接已编码的 `key = value` 条目，生成内联表或逐行的顶层表内容。
pub fn tableText(entries: List<String>, inline: Bool): String {
    var output = if inline { "{" } else { "" }
    for index in 0..<entries.size {
        if index != 0 { output += if inline { ", " } else { "\n" } }
        output += entries[index]
    }
    if inline { output + "}" } else { output }
}

/// 要求节点是表；严格模式下报告首个未知键，不负责检查必需键是否齐全。
pub fn checkFields(value: TomlValue, keys: List<String>, path: String): Result<Unit,String> {
    if !value.isTable() { return Err(path + ": expected table") }
    if value.rejectsUnknownFields {
        for index in 0..<value.size() {
            val key = value.key(index)
            var known = false
            for expected in keys { if key == expected { known = true; break } }
            if !known { return Err(fieldPath(path, key) + ": unknown field") }
        }
    }
    Ok({})
}

/// 按顺序编码列表为 TOML 数组；给每次元素编码传入带零基下标的诊断路径。
pub fn writeArray<T>(values: List<T>, path: String, write: (T, String) -> String): String {
    var output = "["
    for index in 0..<values.size {
        if index != 0 { output += ", " }
        output += write(values[index], path + "[" + index.toString() + "]")
    }
    output + "]"
}

/// 要求是数组节点并按顺序解码，遇到第一项失败即返回其带下标的诊断。
pub fn readArray<T>(value: TomlValue, path: String, read: (TomlValue, String) -> Result<T,String>): Result<List<T>,String> {
    if !value.isArray() { return Err(path + ": expected array") }
    val items: List<T> = []
    for index in 0..<value.size() { items.add(read(value.item(index), path + "[" + index.toString() + "]")?) }
    Ok(items)
}
