extern fn lyraJsonParse(text: String): CPtr;
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
extern fn lyraJsonField(value: CPtr, key: String): CPtr;
extern fn lyraJsonQuote(text: String): String;
extern fn lyraJsonSimpleKey(key: String): Bool;
extern fn lyraJsonIntText(value: Int): String;

pub struct JsonValue(val handle: CPtr, pub val rejectsUnknownFields: Bool) {
    pub fn missing(): Bool { lyraJsonKind(handle) == 0 }
    pub fn isNull(): Bool { lyraJsonKind(handle) == 1 }
    pub fn isObject(): Bool { lyraJsonKind(handle) == 2 }
    pub fn isArray(): Bool { lyraJsonKind(handle) == 3 }
    pub fn size(): Int { lyraJsonSize(handle) }
    pub fn key(index: Int): String { lyraJsonKey(handle, index) }
    pub fn ignoringUnknownFields(): JsonValue { JsonValue(handle, false) }
    pub fn item(index: Int): JsonValue { JsonValue(lyraJsonIndex(handle, index), rejectsUnknownFields) }
    pub fn field(name: String): JsonValue { JsonValue(lyraJsonField(handle, name), rejectsUnknownFields) }
    pub fn readString(path: String): Result<String,String> {
        if lyraJsonKind(handle) == 4 { Ok(lyraJsonText(handle)) } else { Err(path + ": expected String") }
    }
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
    pub fn readInt(path: String): Result<Int,String> { integer(path, "Int") }
    pub fn readInt64(path: String): Result<Int64,String> { integer(path, "Int64") }
    pub fn readInt8(path: String): Result<Int8,String> {
        match (integer(path, "Int8")?).toInt8Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for Int8") }
    }
    pub fn readInt16(path: String): Result<Int16,String> {
        match (integer(path, "Int16")?).toInt16Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for Int16") }
    }
    pub fn readInt32(path: String): Result<Int32,String> {
        match (integer(path, "Int32")?).toInt32Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for Int32") }
    }
    pub fn readUInt8(path: String): Result<UInt8,String> {
        match (integer(path, "UInt8")?).toUInt8Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt8") }
    }
    pub fn readUInt16(path: String): Result<UInt16,String> {
        match (integer(path, "UInt16")?).toUInt16Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt16") }
    }
    pub fn readUInt32(path: String): Result<UInt32,String> {
        match (integer(path, "UInt32")?).toUInt32Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt32") }
    }
    pub fn readFloat32(path: String): Result<Float32,String> {
        val kind = lyraJsonKind(handle)
        if kind == 5 { return Ok(lyraJsonNumber(handle).toFloat32()) }
        if kind != 7 { return Err(path + ": expected Float32") }
        val narrowed = lyraJsonReal(handle).toFloat32()
        if lyraJsonIsFinite(narrowed) { Ok(narrowed) } else { Err(path + ": out of range for Float32") }
    }
    pub fn readFloat64(path: String): Result<Float64,String> {
        val kind = lyraJsonKind(handle)
        if kind == 5 { return Ok(lyraJsonNumber(handle).toFloat64()) }
        if kind == 7 { Ok(lyraJsonReal(handle)) } else { Err(path + ": expected Float64") }
    }
    pub fn readFloat(path: String): Result<Float,String> { readFloat64(path) }
    pub fn readBool(path: String): Result<Bool,String> {
        if lyraJsonKind(handle) == 6 { Ok(lyraJsonNumber(handle) != 0) } else { Err(path + ": expected Bool") }
    }
}

pub fn parseJson(text: String): Result<JsonValue,String> {
    val value = lyraJsonParse(text)
    if lyraJsonKind(value) == 8 { Err(lyraJsonText(value)) } else { Ok(JsonValue(value, true)) }
}
pub fn quoteJson(text: String): String { lyraJsonQuote(text) }
pub fn keyPath(key: String): String { if lyraJsonSimpleKey(key) { "." + key } else { "[" + quoteJson(key) + "]" } }
pub fn intText(value: Int): String { lyraJsonIntText(value) }
pub fn float32Text(value: Float32): String {
    if !lyraJsonIsFinite(value) { throw Exception("JSON cannot encode a non-finite Float32") }
    lyraJsonFloat32Text(value)
}
pub fn float64Text(value: Float64): String {
    if !lyraJsonIsFinite(value) { throw Exception("JSON cannot encode a non-finite Float64") }
    lyraJsonFloat64Text(value)
}
pub fn boolText(value: Bool): String { if value { "true" } else { "false" } }
