extern fn lyraJsonParse(text: String): CPtr;
extern fn lyraJsonKind(value: CPtr): Int;
extern fn lyraJsonSize(value: CPtr): Int;
extern fn lyraJsonNumber(value: CPtr): Int;
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
    pub fn readInt(self, path: String): Result<Int,String> {
        if lyraJsonKind(self.handle) == 5 { Ok(lyraJsonNumber(self.handle)) } else { Err(path + ": expected Int") }
    }
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
pub fn boolText(value: Bool): String { if value { "true" } else { "false" } }
