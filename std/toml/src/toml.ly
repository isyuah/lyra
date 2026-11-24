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

pub struct TomlValue(val handle: CPtr, pub val rejectsUnknownFields: Bool) {
    pub fn missing(): Bool { lyraTomlKind(handle) == 0 }
    pub fn isTable(): Bool { lyraTomlKind(handle) == 1 }
    pub fn isArray(): Bool { lyraTomlKind(handle) == 2 }
    pub fn isTemporal(): Bool { lyraTomlKind(handle) == 7 }
    pub fn size(): Int { lyraTomlSize(handle) }
    pub fn key(index: Int): String { lyraTomlKey(handle, index) }
    pub fn field(key: String): TomlValue { TomlValue(lyraTomlField(handle, key), rejectsUnknownFields) }
    pub fn item(index: Int): TomlValue { TomlValue(lyraTomlItem(handle, index), rejectsUnknownFields) }
    pub fn ignoringUnknownFields(): TomlValue { TomlValue(handle, false) }

    pub fn readString(path: String): Result<String,String> {
        if lyraTomlKind(handle) == 3 { Ok(lyraTomlText(handle)) } else { Err(path + ": expected String") }
    }
    pub fn readBool(path: String): Result<Bool,String> {
        if lyraTomlKind(handle) == 5 { Ok(lyraTomlInteger(handle) != 0) } else { Err(path + ": expected Bool") }
    }
    pub fn readInt(path: String): Result<Int,String> {
        if lyraTomlKind(handle) == 4 { Ok(lyraTomlInteger(handle)) } else { Err(path + ": expected Int") }
    }
    pub fn readInt8(path: String): Result<Int8,String> {
        match (readInt(path)?).toInt8Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for Int8") }
    }
    pub fn readInt16(path: String): Result<Int16,String> {
        match (readInt(path)?).toInt16Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for Int16") }
    }
    pub fn readInt32(path: String): Result<Int32,String> {
        match (readInt(path)?).toInt32Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for Int32") }
    }
    pub fn readUInt8(path: String): Result<UInt8,String> {
        match (readInt(path)?).toUInt8Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt8") }
    }
    pub fn readUInt16(path: String): Result<UInt16,String> {
        match (readInt(path)?).toUInt16Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt16") }
    }
    pub fn readUInt32(path: String): Result<UInt32,String> {
        match (readInt(path)?).toUInt32Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt32") }
    }
    pub fn readUInt64(path: String): Result<UInt64,String> {
        match (readInt(path)?).toUInt64Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt64") }
    }
    pub fn readFloat64(path: String): Result<Float64,String> {
        if lyraTomlKind(handle) == 6 { Ok(lyraTomlFloat(handle)) } else { Err(path + ": expected Float64") }
    }
    pub fn readFloat32(path: String): Result<Float32,String> {
        val number = readFloat64(path)?
        val narrowed = number.toFloat32()
        if lyraTomlIsFinite(number) && !lyraTomlIsFinite(narrowed) { return Err(path + ": out of range for Float32") }
        Ok(narrowed)
    }
}

pub fn parseToml(text: String): Result<TomlValue,String> {
    val handle = lyraTomlParse(text)
    if lyraTomlKind(handle) == 8 { Err(lyraTomlText(handle)) } else { Ok(TomlValue(handle, true)) }
}

pub fn quoteToml(value: String): String { lyraTomlQuote(value) }
pub fn float32Text(value: Float32): String { lyraTomlFloat32Text(value) }
pub fn float64Text(value: Float64): String { lyraTomlFloat64Text(value) }
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

pub fn tableKey(key: String): String { if simpleKey(key) { key } else { quoteToml(key) } }
pub fn fieldPath(path: String, key: String): String {
    if simpleKey(key) { path + "." + key } else { path + "[" + quoteToml(key) + "]" }
}

pub fn tableText(entries: List<String>, inline: Bool): String {
    var output = if inline { "{" } else { "" }
    for index in 0..<entries.size {
        if index != 0 { output += if inline { ", " } else { "\n" } }
        output += entries[index]
    }
    if inline { output + "}" } else { output }
}

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

pub fn writeArray<T>(values: List<T>, path: String, write: (T, String) -> String): String {
    var output = "["
    for index in 0..<values.size {
        if index != 0 { output += ", " }
        output += write(values[index], path + "[" + index.toString() + "]")
    }
    output + "]"
}

pub fn readArray<T>(value: TomlValue, path: String, read: (TomlValue, String) -> Result<T,String>): Result<List<T>,String> {
    if !value.isArray() { return Err(path + ": expected array") }
    val items: List<T> = []
    for index in 0..<value.size() { items.add(read(value.item(index), path + "[" + index.toString() + "]")?) }
    Ok(items)
}
