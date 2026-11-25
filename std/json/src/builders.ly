import std.collections.{HashMap, hashMap}

extern fn lyraJsonNull(): CPtr;
extern fn lyraJsonBool(value: Bool): CPtr;
extern fn lyraJsonInt(value: Int): CPtr;
extern fn lyraJsonFloat(value: Float64): CPtr;
extern fn lyraJsonString(value: String): CPtr;
extern fn lyraJsonContainer(size: Int, object: Bool): CPtr;
extern fn lyraJsonSetEntry(container: CPtr, index: Int, key: String, value: CPtr): Unit;

pub fn jsonNull(): JsonValue { JsonValue(lyraJsonNull(), true) }
pub fn jsonBool(value: Bool): JsonValue { JsonValue(lyraJsonBool(value), true) }
pub fn jsonInt(value: Int): JsonValue { JsonValue(lyraJsonInt(value), true) }
pub fn jsonFloat(value: Float64): JsonValue {
    if !lyraJsonIsFinite(value) { throw Exception("JSON cannot encode a non-finite Float64") }
    JsonValue(lyraJsonFloat(value), true)
}
pub fn jsonString(value: String): JsonValue { JsonValue(lyraJsonString(value), true) }

struct JsonField(val key: String, val value: JsonValue)

pub class JsonObjectBuilder {
    val fields: List<JsonField> = []
    val indexByKey: HashMap<String,Int> = hashMap<String,Int>()

    pub fn put(key: String, value: JsonValue): JsonObjectBuilder {
        checkJsonKey(key)
        requirePresent(value)
        val field = JsonField(key, value)
        match indexByKey.get(key) {
            Some(index) => { fields[index] = field },
            None => { indexByKey.put(key, fields.size); fields.add(field) },
        }
        this
    }

    pub fn build(): JsonValue {
        val result = lyraJsonContainer(fields.size, true)
        for index in 0..<fields.size {
            val field = fields[index]
            lyraJsonSetEntry(result, index, field.key, field.value.handle)
        }
        JsonValue(result, true)
    }
}

pub class JsonArrayBuilder {
    val values: List<JsonValue> = []

    pub fn add(value: JsonValue): JsonArrayBuilder {
        requirePresent(value)
        values.add(value)
        this
    }

    pub fn build(): JsonValue {
        val result = lyraJsonContainer(values.size, false)
        for index in 0..<values.size { lyraJsonSetEntry(result, index, "", values[index].handle) }
        JsonValue(result, true)
    }
}

pub fn jsonObject(): JsonObjectBuilder { JsonObjectBuilder() }
pub fn jsonArray(): JsonArrayBuilder { JsonArrayBuilder() }
