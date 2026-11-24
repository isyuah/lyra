fn jsonInteger(typeName: String): Bool {
    typeName == "Int" || typeName == "Int8" || typeName == "Int16" || typeName == "Int32" || typeName == "Int64" || typeName == "UInt8" || typeName == "UInt16" || typeName == "UInt32"
}

fn encodeValue(typeName: String, expression: String, codec: String, prefix: String): String {
    val element = listElement(typeName)
    if element != "" {
        if optionInner(optionInner(element)) != "" { return macroFail("JSON array items cannot distinguish nested optional states") }
        return "{ val entries = " + expression + "; var arrayText = \"[\"; var pos = 0; while pos < entries.size { if pos != 0 { arrayText += \",\" }; arrayText += " + encodeValue(element, "entries[pos]", codec, prefix) + "; pos += 1 }; arrayText + \"]\" }"
    }
    val inner = optionInner(typeName)
    if inner != "" {
        return "match " + expression + " { None => \"null\", Some(item) => " + encodeValue(inner, "item", codec, prefix) + " }"
    }
    if codec != "" { return codec + ".encode(" + expression + ")" }
    if typeName == "String" { return prefix + "quoteJson(" + expression + ")" }
    if typeName == "Char" { return prefix + "quoteJson((" + expression + ").toString())" }
    if jsonInteger(typeName) { return prefix + "intText(" + expression + ")" }
    if typeName == "Float32" { return prefix + "float32Text(" + expression + ")" }
    if typeName == "Float" || typeName == "Float64" { return prefix + "float64Text(" + expression + ")" }
    if typeName == "UInt" || typeName == "UInt64" { return macroFail("JSON integers are signed 64-bit; UInt/UInt64 requires an explicit mapping") }
    if typeName == "Bool" { return prefix + "boolText(" + expression + ")" }
    "encode" + typeName + "(" + expression + ")"
}

fn readValue(typeName: String, source: String, path: String, codec: String, prefix: String): String {
    val element = listElement(typeName)
    if element != "" {
        return "{ val arrayNode = " + source + "; val arrayPath = " + path + "; if !arrayNode.isArray() { return Err(arrayPath + \": expected array\") }; var pos = 0; val entries: " + typeName + " = []; while pos < arrayNode.size() { val itemNode = arrayNode.item(pos); val itemPath = arrayPath + \"[\" + " + prefix + "intText(pos) + \"]\"; val item: " + element + " = " + readValue(element, "itemNode", "itemPath", codec, prefix) + "; entries.add(item); pos += 1 }; entries }"
    }
    val inner = optionInner(typeName)
    if inner != "" { return "if " + source + ".isNull() { None } else { Some(" + readValue(inner, source, path, codec, prefix) + ") }" }
    if codec != "" { return codec + ".read(" + source + ", " + path + ")?" }
    if typeName == "String" || typeName == "Char" || typeName == "Bool" || jsonInteger(typeName) || typeName == "Float" || typeName == "Float32" || typeName == "Float64" { return source + ".read" + typeName + "(" + path + ")?" }
    "read" + typeName + "Json(" + source + ", " + path + ")?"
}

fn jsonKey(index: Int): String {
    if !fieldHasAttribute(index, "jsonName") { return fieldName(index) }
    val key = fieldAttributeValue(index, "jsonName")
    if key == "" || macroContains(key, "\0") { return macroFail("jsonName on " + fieldName(index) + " requires a non-empty key without NUL") }
    key
}

fn codecLeaf(typeName: String): String {
    val element = listElement(typeName)
    if element != "" { return codecLeaf(element) }
    val inner = optionInner(typeName)
    if inner != "" { return codecLeaf(inner) }
    typeName
}

fn encodeField(name: String, key: String, typeName: String, defaultKind: String, codec: String, prefix: String): String {
    val member = "value.`" + name + "`"
    val encodedKey = if key == name { macroQuote(macroQuote(name) + ":") } else { prefix + "quoteJson(" + macroQuote(key) + ") + \":\"" }
    val append = "if output != \"{\" { output += \",\" }; output += " + encodedKey + " + "
    val inner = optionInner(typeName)
    if inner == "" || defaultKind == "expression" { return append + encodeValue(typeName, member, codec, prefix) + "\n" }
    "match " + member + " { None => {}, Some(item) => { " + append + encodeValue(inner, "item", codec, prefix) + " } }\n"
}

fn readPresentField(typeName: String, source: String, path: String, codec: String, prefix: String): String {
    val inner = optionInner(typeName)
    if optionInner(inner) != "" { return "Some(" + readValue(inner, source, path, codec, prefix) + ")" }
    readValue(typeName, source, path, codec, prefix)
}

fn generatedPrefix(target: String): String {
    var names = target
    var index = 0
    while index < fieldCount() {
        names += "\0" + fieldType(index)
        if fieldHasAttribute(index, "jsonCodec") { names += "\0" + fieldAttributeValue(index, "jsonCodec") }
        index += 1
    }
    var prefix = "__lyraJson"
    while macroContains(names, prefix) { prefix += "_" }
    prefix
}

fn jsonImport(name: String, prefix: String): String { "import std.json." + name + " as " + prefix + name + "\n" }

fn deriveJson(target: String, argument: String): String {
    if argument != "Json" || targetKind() != "struct" { return macroFail("#derive(Json) expects a struct") }
    val prefix = generatedPrefix(target)
    var encoding = "fn encode" + target + "(value: `" + target + "`): String {\nvar output = \"{\"\n"
    var decoding = ""
    var defaults = ""
    var codecHelpers = ""
    var known = "false"
    var seenKeys = "\0"
    var args = ""
    var index = 0
    while index < fieldCount() {
        val name = fieldName(index)
        val defaultKind = fieldDefaultKind(index)
        val slot = "slot" + index.toString()
        if !fieldPublic(index) {
            if defaultKind == "required" { return macroFail("Cannot construct private field without a default: " + name) }
            defaults += "val " + slot + " = " + fieldDefaultFunction(index) + "(" + args + ")\n"
        } else {
            val typeName = fieldType(index)
            val inner = optionInner(typeName)
            val nested = optionInner(inner)
            if optionInner(nested) != "" { return macroFail("JSON cannot distinguish a third optional layer at " + name) }
            if nested != "" && defaultKind == "expression" { return macroFail("Nested optional field " + name + " needs a literal None default or a custom JSON mapping") }
            val key = jsonKey(index)
            if macroContains(seenKeys, "\0" + key + "\0") { return macroFail("Duplicate JSON key " + macroQuote(key) + " at field " + name) }
            seenKeys += key + "\0"
            var codec = ""
            if fieldHasAttribute(index, "jsonCodec") {
                val factory = fieldAttributeValue(index, "jsonCodec")
                if factory == "" || macroContains(factory, "`") { return macroFail("jsonCodec on " + name + " requires a factory identifier") }
                val helper = prefix + "field" + index.toString() + "_" + target
                codecHelpers += "fn " + helper + "(): " + prefix + "JsonCodec<" + codecLeaf(typeName) + "> { `" + factory + "`() }\n"
                codec = "fieldCodec" + index.toString()
                val binding = "val " + codec + " = " + helper + "()\n"
                encoding += binding
                decoding += binding
            }
            encoding += encodeField(name, key, typeName, defaultKind, codec, prefix)
            val quoted = macroQuote(key)
            val source = "node" + index.toString()
            val path = if key == name { "path + " + macroQuote("." + name) } else { "path + " + prefix + "keyPath(" + quoted + ")" }
            known += " || key == " + quoted
            decoding += "val " + source + " = value.field(" + quoted + ")\n"
            var read = readPresentField(typeName, source, path, codec, prefix)
            if defaultKind != "expression" {
                if inner != "" { read = "if " + source + ".missing() { None } else { " + read + " }" }
                decoding += "val " + slot + ": " + typeName + " = " + read + "\n"
            } else {
                val supplied = "supplied" + index.toString()
                decoding += "val " + supplied + ": Option<" + typeName + "> = if " + source + ".missing() { None } else { Some(" + read + ") }\n"
                defaults += "val " + slot + " = " + supplied + " ?? " + fieldDefaultFunction(index) + "(" + args + ")\n"
            }
        }
        if index != 0 { args += ", " }
        args += slot
        index += 1
    }
    val imports = jsonImport("JsonValue", prefix) + jsonImport("JsonCodec", prefix) + jsonImport("jsonCodec", prefix) + jsonImport("parseJson", prefix) + jsonImport("intText", prefix) + jsonImport("float32Text", prefix) + jsonImport("float64Text", prefix) + jsonImport("quoteJson", prefix) + jsonImport("boolText", prefix) + jsonImport("keyPath", prefix)
    val head = "fn read" + target + "Json(value: " + prefix + "JsonValue, path: String): Result<`" + target + "`,String> {\nif !value.isObject() { return Err(path + \": expected object\") }\nif value.rejectsUnknownFields { var index = 0; while index < value.size() { val key = value.key(index); if !(" + known + ") { return Err(path + " + prefix + "keyPath(key) + \": unknown field\") }; index += 1 } }\n"
    val factory = "fn jsonCodec" + target + "(): " + prefix + "JsonCodec<`" + target + "`> { " + prefix + "jsonCodec<`" + target + "`>(encode" + target + ", read" + target + "Json) }\n"
    val decoder = "fn decode" + target + "(text: String): Result<`" + target + "`,String> { read" + target + "Json(" + prefix + "parseJson(text)?, \"$\") }\n"
    val ignoring = "fn decode" + target + "IgnoringUnknownFields(text: String): Result<`" + target + "`,String> { read" + target + "Json((" + prefix + "parseJson(text)?).ignoringUnknownFields(), \"$\") }\n"
    imports + codecHelpers + encoding + "output + \"}\"\n}\n" + head + decoding + defaults + "Ok(`" + target + "`(" + args + "))\n}\n" + factory + decoder + ignoring
}

fn derive(target: String, argument: String): Syntax { syntaxCode(deriveJson(target, argument)) }
