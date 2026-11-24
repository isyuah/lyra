fn tomlScalar(typeName: String): String {
    if typeName == "Int64" { return "Int" }
    if typeName == "UInt" { return "UInt64" }
    if typeName == "Float" { return "Float64" }
    if typeName == "Int" || typeName == "Int8" || typeName == "Int16" || typeName == "Int32" || typeName == "UInt8" || typeName == "UInt16" || typeName == "UInt32" || typeName == "UInt64" || typeName == "Float32" || typeName == "Float64" || typeName == "String" || typeName == "Bool" { return typeName }
    ""
}

fn writeValue(typeName: String, expression: String, path: String, prefix: String): String {
    val scalar = tomlScalar(typeName)
    if scalar == "String" { return prefix + "quoteToml(" + expression + ")" }
    if scalar == "Float32" { return prefix + "float32Text(" + expression + ")" }
    if scalar == "Float64" { return prefix + "float64Text(" + expression + ")" }
    if scalar == "UInt64" { return prefix + "uint64Text(" + expression + ", " + path + ")" }
    if scalar != "" { return expression + ".toString()" }
    val element = listElement(typeName)
    if element != "" {
        return prefix + "writeArray<" + element + ">(" + expression + ", " + path + ", (item, itemPath) => { " + writeValue(element, "item", "itemPath", prefix) + " })"
    }
    if optionInner(typeName) != "" { return macroFail("TOML has no null for nested options or optional array items; use an explicit representation") }
    "write" + typeName + "Toml(" + expression + ", " + path + ", true)"
}

fn readValue(typeName: String, source: String, path: String, prefix: String): String {
    val scalar = tomlScalar(typeName)
    if scalar != "" { return source + ".read" + scalar + "(" + path + ")?" }
    val element = listElement(typeName)
    if element != "" {
        return prefix + "readArray<" + element + ">(" + source + ", " + path + ", (item, itemPath) => { Ok(" + readValue(element, "item", "itemPath", prefix) + ") })?"
    }
    "read" + typeName + "Toml(" + source + ", " + path + ")?"
}

fn tomlImport(name: String, prefix: String): String { "import std.toml." + name + " as " + prefix + name + "\n" }

fn deriveToml(target: String, argument: String): String {
    if targetKind() != "struct" || argument != "Toml" { return macroFail("#derive(Toml) expects a struct") }
    var types = target
    var index = 0
    while index < fieldCount() { types += "\0" + fieldType(index); index += 1 }
    var prefix = "__lyraToml"
    while macroContains(types, prefix) { prefix += "_" }

    var writer = "fn write" + target + "Toml(value: `" + target + "`, path: String, inline: Bool): String {\nval entries: List<String> = []\n"
    var explicit = ""
    var defaults = ""
    var keys = ""
    var seen = "\0"
    var args = ""
    index = 0
    while index < fieldCount() {
        val name = fieldName(index)
        val defaultKind = fieldDefaultKind(index)
        val slot = "slot" + index.toString()
        if !fieldPublic(index) {
            if defaultKind == "required" { return macroFail("Cannot construct private TOML field without a default: " + name) }
            defaults += "val " + slot + " = " + fieldDefaultFunction(index) + "(" + args + ")\n"
        } else {
            val typeName = fieldType(index)
            val key = if fieldHasAttribute(index, "tomlName") { fieldAttributeValue(index, "tomlName") } else { name }
            if key == "" || macroContains(key, "\0") { return macroFail("tomlName requires a non-empty key without NUL: " + name) }
            if macroContains(seen, "\0" + key + "\0") { return macroFail("Duplicate TOML key " + macroQuote(key)) }
            seen += key + "\0"
            if keys != "" { keys += ", " }
            val quoted = macroQuote(key)
            keys += quoted
            val member = "value.`" + name + "`"
            val path = prefix + "fieldPath(path, " + quoted + ")"
            val append = "entries.add(" + prefix + "tableKey(" + quoted + ") + \" = \" + "
            val source = "node" + index.toString()
            explicit += "val " + source + " = value.field(" + quoted + ")\n"
            val inner = optionInner(typeName)
            var read = ""
            if inner == "" {
                writer += append + writeValue(typeName, member, path, prefix) + ")\n"
                read = readValue(typeName, source, path, prefix)
            } else {
                val absent = if defaultKind == "expression" { "throw Exception(" + path + " + \": cannot omit None with a custom TOML default\")" } else { "{}" }
                writer += "match " + member + " { None => " + absent + ", Some(item) => { " + append + writeValue(inner, "item", path, prefix) + ") } }\n"
                read = "Some(" + readValue(inner, source, path, prefix) + ")"
            }
            if defaultKind == "expression" {
                val supplied = "supplied" + index.toString()
                explicit += "val " + supplied + ": Option<" + typeName + "> = if " + source + ".missing() { None } else { Some(" + read + ") }\n"
                defaults += "val " + slot + " = " + supplied + " ?? " + fieldDefaultFunction(index) + "(" + args + ")\n"
            } else {
                if inner != "" { read = "if " + source + ".missing() { None } else { " + read + " }" }
                explicit += "val " + slot + ": " + typeName + " = " + read + "\n"
            }
        }
        if index != 0 { args += ", " }
        args += slot
        index += 1
    }
    val imports = tomlImport("TomlValue", prefix) + tomlImport("parseToml", prefix) + tomlImport("quoteToml", prefix) + tomlImport("tableKey", prefix) + tomlImport("fieldPath", prefix) + tomlImport("checkFields", prefix) + tomlImport("tableText", prefix) + tomlImport("writeArray", prefix) + tomlImport("readArray", prefix) + tomlImport("uint64Text", prefix) + tomlImport("float32Text", prefix) + tomlImport("float64Text", prefix)
    val result = "Result<`" + target + "`,String>"
    writer += prefix + "tableText(entries, inline)\n}\n"
    val reader = "fn read" + target + "Toml(value: " + prefix + "TomlValue, path: String): " + result + " {\n" + prefix + "checkFields(value, [" + keys + "], path)?\n" + explicit + defaults + "Ok(`" + target + "`(" + args + "))\n}\n"
    val encode = "fn encode" + target + "Toml(value: `" + target + "`): String { write" + target + "Toml(value, \"$\", false) }\n"
    val decode = "fn decode" + target + "Toml(text: String): " + result + " { read" + target + "Toml(" + prefix + "parseToml(text)?, \"$\") }\n"
    val loose = "fn decode" + target + "TomlIgnoringUnknownFields(text: String): " + result + " { read" + target + "Toml((" + prefix + "parseToml(text)?).ignoringUnknownFields(), \"$\") }\n"
    imports + writer + reader + encode + decode + loose
}

fn derive(target: String, argument: String): Syntax { syntaxCode(deriveToml(target, argument)) }
