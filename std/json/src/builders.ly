import std.collections.{HashMap, hashMap}

extern fn lyraJsonNull(): CPtr;
extern fn lyraJsonBool(value: Bool): CPtr;
extern fn lyraJsonInt(value: Int): CPtr;
extern fn lyraJsonFloat(value: Float64): CPtr;
extern fn lyraJsonString(value: String): CPtr;
extern fn lyraJsonContainer(size: Int, object: Bool): CPtr;
extern fn lyraJsonSetEntry(container: CPtr, index: Int, key: String, value: CPtr): Unit;

/// 创建显式 JSON `null`，不同于字段缺失。
pub fn jsonNull(): JsonValue { JsonValue(lyraJsonNull(), true) }
/// 创建 JSON 布尔节点，不使用数字或字符串表示。
pub fn jsonBool(value: Bool): JsonValue { JsonValue(lyraJsonBool(value), true) }
/// 创建有符号 64 位 JSON 整数节点。
pub fn jsonInt(value: Int): JsonValue { JsonValue(lyraJsonInt(value), true) }
/// 创建 JSON 浮点节点；NaN 和无穷大无法表示，会抛出异常。
pub fn jsonFloat(value: Float64): JsonValue {
    if !lyraJsonIsFinite(value) { throw Exception("JSON cannot encode a non-finite Float64") }
    JsonValue(lyraJsonFloat(value), true)
}
/// 创建字符串节点，保留原始内容；序列化时自动转义，无需先调用 `quoteJson`。
pub fn jsonString(value: String): JsonValue { JsonValue(lyraJsonString(value), true) }

struct JsonField(val key: String, val value: JsonValue)

/// 可重复使用的对象构建器；用 `build()` 获取当时内容的只读快照。
pub class JsonObjectBuilder {
    val fields: List<JsonField> = []
    val indexByKey: HashMap<String,Int> = hashMap<String,Int>()

    /// 写入字段并返回当前构建器；覆盖不改变首次插入顺序，拒绝含 NUL 的键和缺失节点。
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

    /// 生成按首次插入顺序保存字段的对象，之后的 `put` 不会修改它。
    pub fn build(): JsonValue {
        val result = lyraJsonContainer(fields.size, true)
        for index in 0..<fields.size {
            val field = fields[index]
            lyraJsonSetEntry(result, index, field.key, field.value.handle)
        }
        JsonValue(result, true)
    }
}

/// 按添加顺序积累元素的可变构建器，每次 `build()` 都创建只读数组。
pub class JsonArrayBuilder {
    val values: List<JsonValue> = []

    /// 在末尾追加节点并返回构建器；缺失节点不可加入，请显式使用 `jsonNull()`。
    pub fn add(value: JsonValue): JsonArrayBuilder {
        requirePresent(value)
        values.add(value)
        this
    }

    /// 生成当前元素序列的数组快照，继续追加不影响快照。
    pub fn build(): JsonValue {
        val result = lyraJsonContainer(values.size, false)
        for index in 0..<values.size { lyraJsonSetEntry(result, index, "", values[index].handle) }
        JsonValue(result, true)
    }
}

/// 创建空对象构建器，可链式调用 `put` 后 `build`。
pub fn jsonObject(): JsonObjectBuilder { JsonObjectBuilder() }
/// 创建空数组构建器，可链式调用 `add` 后 `build`。
pub fn jsonArray(): JsonArrayBuilder { JsonArrayBuilder() }
