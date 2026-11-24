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
