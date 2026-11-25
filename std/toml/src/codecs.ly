import std.collections.HashMap
import std.collections.stringMap

pub struct TomlCodec<T>(val writeValue: (T, String, Bool) -> String, val readValue: (TomlValue, String) -> Result<T,String>) {
    pub fn write(value: T, path: String, inline: Bool): String { writeValue(value, path, inline) }
    pub fn read(value: TomlValue, path: String): Result<T,String> { readValue(value, path) }
}

pub fn tomlCodec<T>(write: (T, String, Bool) -> String, read: (TomlValue, String) -> Result<T,String>): TomlCodec<T> {
    TomlCodec(write, read)
}

pub fn tomlCodecString(): TomlCodec<String> { tomlCodec<String>((value, path, inline) => { quoteToml(value) }, (node, path) => { node.readString(path) }) }
pub fn tomlCodecBool(): TomlCodec<Bool> { tomlCodec<Bool>((value, path, inline) => { value.toString() }, (node, path) => { node.readBool(path) }) }
pub fn tomlCodecInt(): TomlCodec<Int> { tomlCodec<Int>((value, path, inline) => { value.toString() }, (node, path) => { node.readInt(path) }) }
pub fn tomlCodecInt8(): TomlCodec<Int8> { tomlCodec<Int8>((value, path, inline) => { value.toString() }, (node, path) => { node.readInt8(path) }) }
pub fn tomlCodecInt16(): TomlCodec<Int16> { tomlCodec<Int16>((value, path, inline) => { value.toString() }, (node, path) => { node.readInt16(path) }) }
pub fn tomlCodecInt32(): TomlCodec<Int32> { tomlCodec<Int32>((value, path, inline) => { value.toString() }, (node, path) => { node.readInt32(path) }) }
pub fn tomlCodecUInt8(): TomlCodec<UInt8> { tomlCodec<UInt8>((value, path, inline) => { value.toString() }, (node, path) => { node.readUInt8(path) }) }
pub fn tomlCodecUInt16(): TomlCodec<UInt16> { tomlCodec<UInt16>((value, path, inline) => { value.toString() }, (node, path) => { node.readUInt16(path) }) }
pub fn tomlCodecUInt32(): TomlCodec<UInt32> { tomlCodec<UInt32>((value, path, inline) => { value.toString() }, (node, path) => { node.readUInt32(path) }) }
pub fn tomlCodecUInt64(): TomlCodec<UInt64> { tomlCodec<UInt64>((value, path, inline) => { uint64Text(value, path) }, (node, path) => { node.readUInt64(path) }) }
pub fn tomlCodecFloat32(): TomlCodec<Float32> { tomlCodec<Float32>((value, path, inline) => { float32Text(value) }, (node, path) => { node.readFloat32(path) }) }
pub fn tomlCodecFloat64(): TomlCodec<Float64> { tomlCodec<Float64>((value, path, inline) => { float64Text(value) }, (node, path) => { node.readFloat64(path) }) }

pub fn tomlCodecList<T>(element: TomlCodec<T>): TomlCodec<List<T>> {
    tomlCodec<List<T>>((items, path, _) => { writeArray<T>(items, path, (item, at) => { element.write(item, at, true) }) },
                       (node, path) => { readArray<T>(node, path, (item, at) => { element.read(item, at) }) })
}

pub fn tomlCodecOptionalField<T>(element: TomlCodec<T>): TomlCodec<Option<T>> {
    tomlCodec<Option<T>>((value, path, inline) => {
        match value {
            None => throw Exception(path + ": cannot omit None with a custom TOML default"),
            Some(item) => element.write(item, path, inline),
        }
    }, (node, path) => { if node.missing() { Ok(None) } else { Ok(Some(element.read(node, path)?)) } })
}

pub fn writeTagged(tag: String, payload: String?, inline: Bool): String {
    val entries = ["tag = " + quoteToml(tag)]
    if let Some(text) = payload { entries.add("value = " + text); }
    tableText(entries, inline)
}

pub fn readTag(value: TomlValue, path: String): Result<String,String> {
    if !value.isTable() { return Err(path + ": expected table") }
    value.field("tag").readString(path + ".tag")
}

pub fn readTaggedValue(value: TomlValue, path: String): Result<TomlValue,String> {
    checkFields(value, ["tag", "value"], path)?
    val item = value.field("value")
    if item.missing() { return Err(path + ".value: missing field") }
    Ok(item)
}

pub fn tomlCodecOption<T>(element: TomlCodec<T>): TomlCodec<Option<T>> {
    tomlCodec<Option<T>>((value, path, inline) => {
        match value {
            None => writeTagged("None", None, inline),
            Some(item) => writeTagged("Some", Some(element.write(item, path + ".value", true)), inline),
        }
    }, (node, path) => {
        val tag = readTag(node, path)?
        match tag {
            "None" => { checkFields(node, ["tag"], path)?; Ok(None) },
            "Some" => Ok(Some(element.read(readTaggedValue(node, path)?, path + ".value")?)),
            _ => Err(path + ".tag: unknown Option tag " + quoteToml(tag)),
        }
    })
}

pub fn tomlCodecResult<T,E>(success: TomlCodec<T>, failure: TomlCodec<E>): TomlCodec<Result<T,E>> {
    tomlCodec<Result<T,E>>((value, path, inline) => {
        match value {
            Ok(item) => writeTagged("Ok", Some(success.write(item, path + ".value", true)), inline),
            Err(error) => writeTagged("Err", Some(failure.write(error, path + ".value", true)), inline),
        }
    }, (node, path) => {
        val tag = readTag(node, path)?
        match tag {
            "Ok" => Ok(Ok(success.read(readTaggedValue(node, path)?, path + ".value")?)),
            "Err" => Ok(Err(failure.read(readTaggedValue(node, path)?, path + ".value")?)),
            _ => Err(path + ".tag: unknown Result tag " + quoteToml(tag)),
        }
    })
}

pub fn tomlCodecStringMap<T>(element: TomlCodec<T>): TomlCodec<HashMap<String,T>> {
    tomlCodec<HashMap<String,T>>((map, path, inline) => {
        val entries: List<String> = []
        for entry in map.entries() {
            entries.add(tableKey(entry.key) + " = " + element.write(entry.value, fieldPath(path, entry.key), true));
        }
        tableText(entries, inline)
    }, (node, path) => {
        if !node.isTable() { return Err(path + ": expected table") }
        val map = stringMap<T>()
        for index in 0..<node.size() {
            val key = node.key(index)
            map.put(key, element.read(node.field(key), fieldPath(path, key))?);
        }
        Ok(map)
    })
}
