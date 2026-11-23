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

pub struct JsonValue(val handle: CPtr) {
    pub fn missing(self): Bool { lyraJsonKind(self.handle) == 0 }
    pub fn isNull(self): Bool { lyraJsonKind(self.handle) == 1 }
    pub fn isObject(self): Bool { lyraJsonKind(self.handle) == 2 }
    pub fn isArray(self): Bool { lyraJsonKind(self.handle) == 3 }
    pub fn size(self): Int { lyraJsonSize(self.handle) }
    pub fn key(self, index: Int): String { lyraJsonKey(self.handle, index) }
    pub fn item(self, index: Int): JsonValue { JsonValue(lyraJsonIndex(self.handle, index)) }
    pub fn field(self, name: String): JsonValue { JsonValue(lyraJsonField(self.handle, name)) }
    pub fn readString(self, path: String): Result<String,String> {
        if lyraJsonKind(self.handle) == 4 { Ok(lyraJsonText(self.handle)) } else { Err(path + ": expected String") }
    }
    fn integer(self, path: String, name: String): Result<Int,String> {
        if lyraJsonKind(self.handle) == 5 { Ok(lyraJsonNumber(self.handle)) } else { Err(path + ": expected " + name) }
    }
    pub fn readInt(self, path: String): Result<Int,String> { self.integer(path, "Int") }
    pub fn readInt64(self, path: String): Result<Int64,String> { self.integer(path, "Int64") }
    pub fn readInt8(self, path: String): Result<Int8,String> {
        match (self.integer(path, "Int8")?).toInt8Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for Int8") }
    }
    pub fn readInt16(self, path: String): Result<Int16,String> {
        match (self.integer(path, "Int16")?).toInt16Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for Int16") }
    }
    pub fn readInt32(self, path: String): Result<Int32,String> {
        match (self.integer(path, "Int32")?).toInt32Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for Int32") }
    }
    pub fn readUInt8(self, path: String): Result<UInt8,String> {
        match (self.integer(path, "UInt8")?).toUInt8Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt8") }
    }
    pub fn readUInt16(self, path: String): Result<UInt16,String> {
        match (self.integer(path, "UInt16")?).toUInt16Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt16") }
    }
    pub fn readUInt32(self, path: String): Result<UInt32,String> {
        match (self.integer(path, "UInt32")?).toUInt32Checked() { Some(value) => Ok(value), None => Err(path + ": out of range for UInt32") }
    }
    pub fn readFloat32(self, path: String): Result<Float32,String> {
        val kind = lyraJsonKind(self.handle)
        if kind == 5 { return Ok(lyraJsonNumber(self.handle).toFloat32()) }
        if kind != 7 { return Err(path + ": expected Float32") }
        val narrowed = lyraJsonReal(self.handle).toFloat32()
        if lyraJsonIsFinite(narrowed) { Ok(narrowed) } else { Err(path + ": out of range for Float32") }
    }
    pub fn readFloat64(self, path: String): Result<Float64,String> {
        val kind = lyraJsonKind(self.handle)
        if kind == 5 { return Ok(lyraJsonNumber(self.handle).toFloat64()) }
        if kind == 7 { Ok(lyraJsonReal(self.handle)) } else { Err(path + ": expected Float64") }
    }
    pub fn readFloat(self, path: String): Result<Float,String> { self.readFloat64(path) }
    pub fn readBool(self, path: String): Result<Bool,String> {
        if lyraJsonKind(self.handle) == 6 { Ok(lyraJsonNumber(self.handle) != 0) } else { Err(path + ": expected Bool") }
    }
}

pub fn parseJson(text: String): Result<JsonValue,String> {
    val value = lyraJsonParse(text)
    if lyraJsonKind(value) == 8 { Err(lyraJsonText(value)) } else { Ok(JsonValue(value)) }
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
