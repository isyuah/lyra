fn encodeValue(typeName: String, expression: String): String {
    if typeName == "String" { return "quoteJson(" + expression + ")" }
    if typeName == "Int" { return "intText(" + expression + ")" }
    if typeName == "Bool" { return "boolText(" + expression + ")" }
    val inner = optionInner(typeName)
    if inner != "" {
        return "match " + expression + " { None => \"null\", Some(item) => " + encodeValue(inner, "item") + " }"
    }
    "encode" + typeName + "(" + expression + ")"
}

fn readValue(typeName: String, source: String, path: String): String {
    if typeName == "String" || typeName == "Int" || typeName == "Bool" { return source + ".read" + typeName + "(" + path + ")?" }
    val inner = optionInner(typeName)
    if inner != "" { return "if " + source + ".isNull() { None } else { Some(" + readValue(inner, source, path) + ") }" }
    "read" + typeName + "Json(" + source + ", " + path + ")?"
}

fn derive(target: String, argument: String): String {
    if argument != "Json" || targetKind() != "struct" { return macroFail("#derive(Json) expects a struct") }
    var encoding = "fn encode" + target + "(value: `" + target + "`): String {\nvar output = \"{\"\n"
    var decoding = ""
    var known = "false"
    var args = ""
    var index = 0
    while index < fieldCount() {
        val name = fieldName(index)
        val typeName = fieldType(index)
        if !fieldPublic(index) { return macroFail("Cannot construct private field without a default: " + name) }
        val inner = optionInner(typeName)
        if optionInner(optionInner(inner)) != "" { return macroFail("JSON cannot distinguish a third optional layer at " + name) }
        val quoted = macroQuote(name)
        val member = "value.`" + name + "`"
        val prefix = "if output != \"{\" { output += \",\" }; output += " + macroQuote(macroQuote(name) + ":") + " + "
        if inner == "" { encoding += prefix + encodeValue(typeName, member) + "\n" }
        else { encoding += "match " + member + " { None => {}, Some(item) => { " + prefix + encodeValue(inner, "item") + " } }\n" }
        val slot = "slot" + name
        val source = "node" + name
        val path = "path + " + macroQuote("." + name)
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
    val imports = "import std.json.JsonValue\nimport std.json.parseJson\nimport std.json.intText\nimport std.json.quoteJson\nimport std.json.boolText\n"
    val head = "fn read" + target + "Json(value: JsonValue, path: String): Result<`" + target + "`,String> {\nif !value.isObject() { return Err(path + \": expected object\") }\nvar index = 0\nwhile index < value.size() { val key = value.key(index); if !(" + known + ") { return Err(path + \".\" + key + \": unknown field\") }; index += 1 }\n"
    val decoder = "fn decode" + target + "(text: String): Result<`" + target + "`,String> { read" + target + "Json(parseJson(text)?, \"$\") }\n"
    imports + encoding + "output + \"}\"\n}\n" + head + decoding + "Ok(`" + target + "`(" + args + "))\n}\n" + decoder
}
