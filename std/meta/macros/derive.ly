fn jsonInteger(typeName: String): Bool {
    typeName == "Int" || typeName == "Int8" || typeName == "Int16" || typeName == "Int32" || typeName == "Int64" || typeName == "UInt8" || typeName == "UInt16" || typeName == "UInt32"
}

fn encodeValue(typeName: String, expression: String): String {
    if typeName == "String" { return "quoteJson(" + expression + ")" }
    if jsonInteger(typeName) { return "intText(" + expression + ")" }
    if typeName == "Float32" { return "float32Text(" + expression + ")" }
    if typeName == "Float" || typeName == "Float64" { return "float64Text(" + expression + ")" }
    if typeName == "UInt" || typeName == "UInt64" { return macroFail("JSON integers are signed 64-bit; UInt/UInt64 requires an explicit mapping") }
    if typeName == "Bool" { return "boolText(" + expression + ")" }
    val element = listElement(typeName)
    if element != "" {
        if optionInner(optionInner(element)) != "" { return macroFail("JSON array items cannot distinguish nested optional states") }
        return "{ val entries = " + expression + "; var arrayText = \"[\"; var pos = 0; while pos < entries.size { if pos != 0 { arrayText += \",\" }; arrayText += " + encodeValue(element, "entries[pos]") + "; pos += 1 }; arrayText + \"]\" }"
    }
    val inner = optionInner(typeName)
    if inner != "" {
        return "match " + expression + " { None => \"null\", Some(item) => " + encodeValue(inner, "item") + " }"
    }
    "encode" + typeName + "(" + expression + ")"
}

fn readValue(typeName: String, source: String, path: String): String {
    if typeName == "String" || typeName == "Bool" || jsonInteger(typeName) || typeName == "Float" || typeName == "Float32" || typeName == "Float64" { return source + ".read" + typeName + "(" + path + ")?" }
    val element = listElement(typeName)
    if element != "" {
        return "{ val arrayNode = " + source + "; val arrayPath = " + path + "; if !arrayNode.isArray() { return Err(arrayPath + \": expected array\") }; var pos = 0; val entries: " + typeName + " = []; while pos < arrayNode.size() { val itemNode = arrayNode.item(pos); val itemPath = arrayPath + \"[\" + intText(pos) + \"]\"; val item: " + element + " = " + readValue(element, "itemNode", "itemPath") + "; entries.add(item); pos += 1 }; entries }"
    }
    val inner = optionInner(typeName)
    if inner != "" { return "if " + source + ".isNull() { None } else { Some(" + readValue(inner, source, path) + ") }" }
    "read" + typeName + "Json(" + source + ", " + path + ")?"
}

fn jsonKey(index: Int): String {
    if !fieldHasAttribute(index, "jsonName") { return fieldName(index) }
    val key = fieldAttributeValue(index, "jsonName")
    if key == "" || macroContains(key, "\0") { return macroFail("jsonName on " + fieldName(index) + " requires a non-empty key without NUL") }
    key
}

fn derive(target: String, argument: String): String {
    if argument != "Json" || targetKind() != "struct" { return macroFail("#derive(Json) expects a struct") }
    var encoding = "fn encode" + target + "(value: `" + target + "`): String {\nvar output = \"{\"\n"
    var decoding = ""
    var known = "false"
    var seenKeys = "\0"
    var args = ""
    var index = 0
    while index < fieldCount() {
        val name = fieldName(index)
        val typeName = fieldType(index)
        if !fieldPublic(index) { return macroFail("Cannot construct private field without a default: " + name) }
        val inner = optionInner(typeName)
        if optionInner(optionInner(inner)) != "" { return macroFail("JSON cannot distinguish a third optional layer at " + name) }
        val key = jsonKey(index)
        if macroContains(seenKeys, "\0" + key + "\0") { return macroFail("Duplicate JSON key " + macroQuote(key) + " at field " + name) }
        seenKeys += key + "\0"
        val quoted = macroQuote(key)
        val member = "value.`" + name + "`"
        val encodedKey = if key == name { macroQuote(macroQuote(name) + ":") } else { "quoteJson(" + quoted + ") + \":\"" }
        val prefix = "if output != \"{\" { output += \",\" }; output += " + encodedKey + " + "
        if inner == "" { encoding += prefix + encodeValue(typeName, member) + "\n" }
        else { encoding += "match " + member + " { None => {}, Some(item) => { " + prefix + encodeValue(inner, "item") + " } }\n" }
        val slot = "slot" + name
        val source = "node" + name
        val path = if key == name { "path + " + macroQuote("." + name) } else { "path + keyPath(" + quoted + ")" }
        known += " || key == " + quoted
        decoding += "val " + source + " = value.field(" + quoted + ")\nval " + slot + ": " + typeName + " = "
        if inner == "" { decoding += readValue(typeName, source, path) }
        else {
            var empty = source + ".missing()"
            if optionInner(inner) == "" { empty += " || " + source + ".isNull()" }
            decoding += "if " + empty + " { None } else { Some(" + readValue(inner, source, path) + ") }"
        }
        decoding += "\n"
        if index != 0 { args += ", " }
        args += slot
        index += 1
    }
    val imports = "import std.json.JsonValue\nimport std.json.parseJson\nimport std.json.intText\nimport std.json.float32Text\nimport std.json.float64Text\nimport std.json.quoteJson\nimport std.json.boolText\nimport std.json.keyPath\n"
    val head = "fn read" + target + "Json(value: JsonValue, path: String): Result<`" + target + "`,String> {\nif !value.isObject() { return Err(path + \": expected object\") }\nvar index = 0\nwhile index < value.size() { val key = value.key(index); if !(" + known + ") { return Err(path + keyPath(key) + \": unknown field\") }; index += 1 }\n"
    val decoder = "fn decode" + target + "(text: String): Result<`" + target + "`,String> { read" + target + "Json(parseJson(text)?, \"$\") }\n"
    imports + encoding + "output + \"}\"\n}\n" + head + decoding + "Ok(`" + target + "`(" + args + "))\n}\n" + decoder
}
