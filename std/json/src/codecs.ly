import std.collections.HashMap
import std.collections.stringMap

pub struct JsonCodec<T>(val encodeValue: (T) -> String, val readValue: (JsonValue, String) -> Result<T,String>) {
    pub fn encode(value: T): String { encodeValue(value) }
    pub fn read(value: JsonValue, path: String): Result<T,String> { readValue(value, path) }

    pub fn decode(text: String): Result<T,String> { read(parseJson(text)?, "$") }
    pub fn decodeIgnoringUnknownFields(text: String): Result<T,String> {
        read((parseJson(text)?).ignoringUnknownFields(), "$")
    }
}

pub fn jsonCodec<T>(encode: (T) -> String, read: (JsonValue, String) -> Result<T,String>): JsonCodec<T> {
    JsonCodec(encode, read)
}

pub fn jsonCodecString(): JsonCodec<String> { jsonCodec<String>(quoteJson, (value, path) => { value.readString(path) }) }
pub fn jsonCodecChar(): JsonCodec<Char> { jsonCodec<Char>((value) => { quoteJson(value.toString()) }, (value, path) => { value.readChar(path) }) }
pub fn jsonCodecInt(): JsonCodec<Int> { jsonCodec<Int>(intText, (value, path) => { value.readInt(path) }) }
pub fn jsonCodecInt64(): JsonCodec<Int64> { jsonCodec<Int64>(intText, (value, path) => { value.readInt64(path) }) }
pub fn jsonCodecInt8(): JsonCodec<Int8> { jsonCodec<Int8>((value) => { intText(value) }, (node, path) => { node.readInt8(path) }) }
pub fn jsonCodecInt16(): JsonCodec<Int16> { jsonCodec<Int16>((value) => { intText(value) }, (node, path) => { node.readInt16(path) }) }
pub fn jsonCodecInt32(): JsonCodec<Int32> { jsonCodec<Int32>((value) => { intText(value) }, (node, path) => { node.readInt32(path) }) }
pub fn jsonCodecUInt8(): JsonCodec<UInt8> { jsonCodec<UInt8>((value) => { intText(value) }, (node, path) => { node.readUInt8(path) }) }
pub fn jsonCodecUInt16(): JsonCodec<UInt16> { jsonCodec<UInt16>((value) => { intText(value) }, (node, path) => { node.readUInt16(path) }) }
pub fn jsonCodecUInt32(): JsonCodec<UInt32> { jsonCodec<UInt32>((value) => { intText(value) }, (node, path) => { node.readUInt32(path) }) }
pub fn jsonCodecBool(): JsonCodec<Bool> { jsonCodec<Bool>(boolText, (value, path) => { value.readBool(path) }) }
pub fn jsonCodecFloat32(): JsonCodec<Float32> { jsonCodec<Float32>(float32Text, (value, path) => { value.readFloat32(path) }) }
pub fn jsonCodecFloat64(): JsonCodec<Float64> { jsonCodec<Float64>(float64Text, (value, path) => { value.readFloat64(path) }) }
pub fn jsonCodecUnit(): JsonCodec<Unit> {
    jsonCodec<Unit>((_) => { "null" }, (value, path) => { if value.isNull() { Ok({}) } else { Err(path + ": expected null") } })
}

pub fn jsonCodecNullable<T>(element: JsonCodec<T>): JsonCodec<Option<T>> {
    jsonCodec<Option<T>>((value) => { match value { None => "null", Some(item) => element.encode(item) } },
                         (node, path) => { if node.isNull() { Ok(None) } else { Ok(Some(element.read(node, path)?)) } })
}

pub fn jsonCodecOptionalField<T>(element: JsonCodec<T>): JsonCodec<Option<T>> {
    jsonCodec<Option<T>>((value) => { match value { None => "null", Some(item) => element.encode(item) } },
                         (node, path) => { if node.missing() { Ok(None) } else { Ok(Some(element.read(node, path)?)) } })
}

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

pub fn jsonCodecStringMap<T>(element: JsonCodec<T>): JsonCodec<HashMap<String,T>> {
    jsonCodec<HashMap<String,T>>((map) => {
        var text = "{"
        for entry in map.entries() {
            for byte in entry.key.toBytes() {
                if byte == 0 { throw Exception("JSON object keys cannot contain NUL: " + quoteJson(entry.key)) }
            }
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
