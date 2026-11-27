fn dataImport(name: String, prefix: String, json: Bool): String {
    "import std." + (if json { "json.{" } else { "toml.{" }) + name + " as " + prefix + name + "}\n"
}

fn isType(node: Syntax, name: String): Bool { syntaxKind(node) == "builtin" && syntaxText(node) == name }

fn codecParameters(prefix: String, codecType: String): String {
    var parameters = ""
    var index = 0
    while index < typeParameterCount() {
        if index != 0 { parameters += ", " }
        parameters += prefix + "Type" + index.toString() + ": " + codecType + "<`" + typeParameterName(index) + "`>"
        index += 1
    }
    parameters
}

fn codecArguments(prefix: String): String {
    var arguments = ""
    var index = 0
    while index < typeParameterCount() {
        if index != 0 { arguments += ", " }
        arguments += prefix + "Type" + index.toString()
        index += 1
    }
    arguments
}

fn codecFor(typeNode: Syntax, factory: String, prefix: String, optionMode: String, json: Bool): String {
    val name = syntaxText(typeNode)
    val api = prefix + (if json { "jsonCodec" } else { "tomlCodec" })
    if isType(typeNode, "Option") {
        val inner = syntaxChild(typeNode, 0)
        if optionMode == "tagged" { return api + "Option(" + codecFor(inner, factory, prefix, optionMode, json) + ")" }
        if optionMode == "field" { return api + "OptionalField(" + codecFor(inner, factory, prefix, "nullable", json) + ")" }
        if !json { return macroFail("TOML has no null for nested options or optional array items; use an explicit representation") }
        if isType(inner, "Option") { return macroFail("JSON array items cannot distinguish nested optional states") }
        return api + "Nullable(" + codecFor(inner, factory, prefix, optionMode, json) + ")"
    }
    if isType(typeNode, "List") { return api + "List(" + codecFor(syntaxChild(typeNode, 0), factory, prefix, if optionMode == "field" { "nullable" } else { optionMode }, json) + ")" }
    if factory != "" { return "`" + factory + "`()" }
    if isType(typeNode, "Result") {
        return api + "Result(" + codecFor(syntaxChild(typeNode, 0), "", prefix, "tagged", json) + ", " + codecFor(syntaxChild(typeNode, 1), "", prefix, "tagged", json) + ")"
    }
    if name == "HashMap" && syntaxKind(typeNode) == "standard" && syntaxTypeModule(typeNode) == "std.collections" {
        if !isType(syntaxChild(typeNode, 0), "String") { return macroFail("Map keys must be String; use a custom codec") }
        return api + "StringMap(" + codecFor(syntaxChild(typeNode, 1), "", prefix, "tagged", json) + ")"
    }
    if syntaxKind(typeNode) == "parameter" {
        var index = 0
        while index < typeParameterCount() {
            if typeParameterName(index) == name { return prefix + "Type" + index.toString() }
            index += 1
        }
    }
    if syntaxKind(typeNode) == "builtin" {
        if name == "UInt" || name == "UInt64" {
            if json { return macroFail("JSON integers are signed 64-bit; UInt/UInt64 requires an explicit mapping") }
            return api + "UInt64()"
        }
        if name == "Float" { return api + "Float64()" }
        if name == "Int64" && !json { return api + "Int()" }
        if name == "String" || (json && (name == "Char" || name == "Unit")) || name == "Bool" || name == "Float32" || name == "Float64" || name == "Int" || name == "Int8" || name == "Int16" || name == "Int32" || name == "Int64" || name == "UInt8" || name == "UInt16" || name == "UInt32" {
            return api + name + "()"
        }
    }
    if syntaxKind(typeNode) == "named" || syntaxKind(typeNode) == "standard" {
        if syntaxChildCount(typeNode) == 0 { return if json { api + "(encode" + name + ", read" + name + "Json)" } else { api + "(write" + name + "Toml, read" + name + "Toml)" } }
        var arguments = ""
        var index = 0
        while index < syntaxChildCount(typeNode) {
            if index != 0 { arguments += ", " }
            arguments += codecFor(syntaxChild(typeNode, index), "", prefix, "tagged", json)
            index += 1
        }
        return (if json { "jsonCodec" } else { "tomlCodec" }) + name + "(" + arguments + ")"
    }
    macroFail("No codec for " + name + "; supply a custom codec")
}

fn fieldKey(index: Int, attribute: String): String {
    if !fieldHasAttribute(index, attribute) { return fieldName(index) }
    val key = fieldAttributeValue(index, attribute)
    if key == "" || macroContains(key, "\0") { return macroFail(attribute + " on " + fieldName(index) + " requires a non-empty key without NUL") }
    key
}

fn jsonImports(prefix: String): String {
    dataImport("JsonValue", prefix, true) + dataImport("JsonCodec", prefix, true) + dataImport("jsonCodec", prefix, true) + dataImport("parseJson", prefix, true) + dataImport("quoteJson", prefix, true) + dataImport("keyPath", prefix, true) + dataImport("checkFields", prefix, true) + dataImport("readTag", prefix, true) + dataImport("readTaggedValue", prefix, true) +
    dataImport("jsonCodecString", prefix, true) + dataImport("jsonCodecChar", prefix, true) + dataImport("jsonCodecBool", prefix, true) + dataImport("jsonCodecUnit", prefix, true) + dataImport("jsonCodecFloat32", prefix, true) + dataImport("jsonCodecFloat64", prefix, true) +
    dataImport("jsonCodecInt", prefix, true) + dataImport("jsonCodecInt8", prefix, true) + dataImport("jsonCodecInt16", prefix, true) + dataImport("jsonCodecInt32", prefix, true) + dataImport("jsonCodecInt64", prefix, true) + dataImport("jsonCodecUInt8", prefix, true) + dataImport("jsonCodecUInt16", prefix, true) + dataImport("jsonCodecUInt32", prefix, true) +
    dataImport("jsonCodecList", prefix, true) + dataImport("jsonCodecOptionalField", prefix, true) + dataImport("jsonCodecNullable", prefix, true) + dataImport("jsonCodecOption", prefix, true) + dataImport("jsonCodecResult", prefix, true) + dataImport("jsonCodecStringMap", prefix, true)
}

fn tomlImports(prefix: String): String {
    dataImport("TomlValue", prefix, false) + dataImport("TomlCodec", prefix, false) + dataImport("tomlCodec", prefix, false) + dataImport("parseToml", prefix, false) + dataImport("quoteToml", prefix, false) + dataImport("fieldPath", prefix, false) + dataImport("tableKey", prefix, false) + dataImport("tableText", prefix, false) + dataImport("checkFields", prefix, false) + dataImport("readTag", prefix, false) + dataImport("readTaggedValue", prefix, false) + dataImport("writeTagged", prefix, false) +
    dataImport("tomlCodecString", prefix, false) + dataImport("tomlCodecBool", prefix, false) + dataImport("tomlCodecFloat32", prefix, false) + dataImport("tomlCodecFloat64", prefix, false) +
    dataImport("tomlCodecInt", prefix, false) + dataImport("tomlCodecInt8", prefix, false) + dataImport("tomlCodecInt16", prefix, false) + dataImport("tomlCodecInt32", prefix, false) + dataImport("tomlCodecUInt8", prefix, false) + dataImport("tomlCodecUInt16", prefix, false) + dataImport("tomlCodecUInt32", prefix, false) + dataImport("tomlCodecUInt64", prefix, false) +
    dataImport("tomlCodecList", prefix, false) + dataImport("tomlCodecOptionalField", prefix, false) + dataImport("tomlCodecOption", prefix, false) + dataImport("tomlCodecResult", prefix, false) + dataImport("tomlCodecStringMap", prefix, false)
}

fn wrappers(target: String, prefix: String, format: String): String {
    val json = format == "Json"
    val declaration = typeParameterDeclarations()
    val arguments = typeParameterArguments()
    val typeName = "`" + target + "`" + arguments
    val codecType = prefix + format + "Codec"
    val parameters = codecParameters(prefix, codecType)
    val codecs = codecArguments(prefix)
    val more = if codecs == "" { "" } else { ", " + codecs }
    val moreParams = if parameters == "" { "" } else { ", " + parameters }
    val reader = "read" + target + format
    val writer = if json { "encode" + target } else { "write" + target + "Toml" }
    val codec = (if json { "json" } else { "toml" }) + "Codec"
    val writerClosure = if json { "(item) => { " + writer + arguments + "(item" + more + ") }" } else { "(item, path, inline) => { " + writer + arguments + "(item, path, inline" + more + ") }" }
    val pair = if codecs == "" { writer + ", " + reader } else { writerClosure + ", (node, path) => { " + reader + arguments + "(node, path" + more + ") }" }
    val factory = "fn " + codec + target + declaration + "(" + parameters + "): " + codecType + "<" + typeName + "> { " + prefix + codec + "<" + typeName + ">(" + pair + ") }\n"
    val suffix = if json { "" } else { "Toml" }
    val decoder = "fn decode" + target + suffix + declaration + "(text: String" + moreParams + "): Result<" + typeName + ",String> { " + reader + arguments + "(" + prefix + "parse" + format + "(text)?, \"$\"" + more + ") }\n"
    val loose = "fn decode" + target + suffix + "IgnoringUnknownFields" + declaration + "(text: String" + moreParams + "): Result<" + typeName + ",String> { " + reader + arguments + "((" + prefix + "parse" + format + "(text)?).ignoringUnknownFields(), \"$\"" + more + ") }\n"
    val encode = if json { "" } else { "fn encode" + target + "Toml" + declaration + "(value: " + typeName + moreParams + "): String { " + writer + arguments + "(value, \"$\", false" + more + ") }\n" }
    factory + encode + decoder + loose
}

fn deriveStruct(target: String, prefix: String, format: String): Syntax {
    val json = format == "Json"
    val arguments = typeParameterArguments()
    val declaration = typeParameterDeclarations()
    val typeName = "`" + target + "`" + arguments
    val codecType = prefix + format + "Codec"
    val parameters = codecParameters(prefix, codecType)
    val codecs = codecArguments(prefix)
    val moreParams = if parameters == "" { "" } else { ", " + parameters }
    var encoding = if json { "fn encode" + target + declaration + "(value: " + typeName + moreParams + "): String {\nvar output = \"{\"\n" }
        else { "fn write" + target + "Toml" + declaration + "(value: " + typeName + ", path: String, inline: Bool" + moreParams + "): String {\nval entries: List<String> = []\n" }
    var helpers = syntaxCode(if json { jsonImports(prefix) } else { tomlImports(prefix) })
    var decoding = ""
    var defaults = ""
    var keys = ""
    var seenKeys = "\0"
    var args = ""
    var index = 0
    while index < fieldCount() {
        val name = fieldName(index)
        val defaultKind = fieldDefaultKind(index)
        val slot = "slot" + index.toString()
        val defaultArgs = arguments + "(" + args + ")\n"
        if !fieldPublic(index) {
            val privateField = if json { "private field" } else { "private TOML field" }
            if defaultKind == "required" { return syntaxError(syntaxTarget(), "Cannot construct " + privateField + " without a default: " + name) }
            defaults += "val " + slot + " = " + fieldDefaultFunction(index) + defaultArgs
        } else {
            val field = syntaxChild(syntaxTarget(), index + 1)
            val annotation = syntaxChild(field, 1)
            val typeNode = syntaxResolvedType(annotation)
            val optional = isType(typeNode, "Option")
            var nested = false
            if optional && json {
                val inner = syntaxChild(typeNode, 0)
                if isType(inner, "Option") {
                    nested = true
                    if isType(syntaxChild(inner, 0), "Option") { return syntaxError(field, "JSON cannot distinguish a third optional layer at " + name) }
                    if defaultKind == "expression" { return syntaxError(field, "Nested optional field " + name + " needs a literal None default or a custom JSON mapping") }
                }
            }
            val key = fieldKey(index, if json { "jsonName" } else { "tomlName" })
            val formatName = if json { "JSON" } else { "TOML" }
            if macroContains(seenKeys, "\0" + key + "\0") { return syntaxError(syntaxCode(""), "Duplicate " + formatName + " key " + macroQuote(key) + " at field " + name) }
            seenKeys += key + "\0"
            val quoted = macroQuote(key)
            if keys != "" { keys += ", " }
            keys += quoted
            var factory = ""
            val attribute = if json { "jsonCodec" } else { "tomlCodec" }
            if fieldHasAttribute(index, attribute) {
                factory = fieldAttributeValue(index, attribute)
                if factory == "" || macroContains(factory, "`") { return syntaxError(field, attribute + " on " + name + " requires a factory identifier") }
            }
            val originalType = syntaxText(annotation)
            val helper = prefix + target + "_" + name
            val mode = if nested || (optional && !json) { "field" } else { "nullable" }
            val spec = codecFor(typeNode, factory, prefix, mode, json)
            helpers = syntaxConcat(helpers, syntaxCodeAt(annotation, "fn " + helper + declaration + "(" + parameters + "): " + codecType + "<" + originalType + "> { " + spec + " }\n"))
            val use = helper + arguments + "(" + codecs + ")"
            val member = "value.`" + name + "`"
            val path = if json { "path + " + prefix + "keyPath(" + quoted + ")" } else { prefix + "fieldPath(path, " + quoted + ")" }
            val append = if json { "if output != \"{\" { output += \",\" }; output += " + prefix + "quoteJson(" + quoted + ") + \":\" + " + use + ".encode(" + member + ")" }
                else { "entries.add(" + prefix + "tableKey(" + quoted + ") + \" = \" + " + use + ".write(" + member + ", " + path + ", true))" }
            if optional && defaultKind != "expression" { encoding += "match " + member + " { None => {}, Some(_) => { " + append + " } }\n" }
            else { encoding += append + "\n" }
            val source = "node" + index.toString()
            decoding += "val " + source + " = value.field(" + quoted + ")\n"
            val read = use + ".read(" + source + ", " + path + ")?"
            if defaultKind == "expression" {
                val supplied = "supplied" + index.toString()
                decoding += "val " + supplied + ": Option<" + originalType + "> = if " + source + ".missing() { None } else { Some(" + read + ") }\n"
                defaults += "val " + slot + " = " + supplied + " ?? " + fieldDefaultFunction(index) + defaultArgs
            } else {
                val present = if optional { "if " + source + ".missing() { None } else { " + read + " }" } else { read }
                decoding += "val " + slot + ": " + originalType + " = " + present + "\n"
            }
        }
        if index != 0 { args += ", " }
        args += slot
        index += 1
    }
    val head = "fn read" + target + format + declaration + "(value: " + prefix + format + "Value, path: String" + moreParams + "): Result<" + typeName + ",String> {\n" + prefix + "checkFields(value, [" + keys + "], path)?\n"
    val tail = if json { "output + \"}\"" } else { prefix + "tableText(entries, inline)" }
    syntaxConcat(helpers, syntaxCode(encoding + tail + "\n}\n" + head + decoding + defaults + "Ok(" + typeName + "(" + args + "))\n}\n" + wrappers(target, prefix, format)))
}

fn deriveEnum(target: String, prefix: String, format: String): Syntax {
    val json = format == "Json"
    val arguments = typeParameterArguments()
    val declaration = typeParameterDeclarations()
    val owner = "`" + target + "`"
    val typeName = owner + arguments
    val codecType = prefix + format + "Codec"
    val parameters = codecParameters(prefix, codecType)
    val codecs = codecArguments(prefix)
    val moreParams = if parameters == "" { "" } else { ", " + parameters }
    val writer = if json { "encode" + target } else { "write" + target + "Toml" }
    val writeParams = if json { "" } else { ", path: String, inline: Bool" }
    var encoding = "fn " + writer + declaration + "(value: " + typeName + writeParams + moreParams + "): String {\nmatch value {\n"
    var decoding = "fn read" + target + format + declaration + "(value: " + prefix + format + "Value, path: String" + moreParams + "): Result<" + typeName + ",String> {\nmatch " + prefix + "readTag(value, path)? {\n"
    var helpers = syntaxCode(if json { jsonImports(prefix) } else { tomlImports(prefix) })
    val root = syntaxTarget()
    var index = 1
    while index < syntaxChildCount(root) {
        val entry = syntaxChild(root, index)
        val name = syntaxIdentifier(syntaxChild(entry, 0))
        val member = ".`" + name + "`"
        val tagText = macroQuote("{\"tag\":") + " + " + prefix + "quoteJson(" + macroQuote(name) + ")"
        val count = syntaxChildCount(entry) - 1
        decoding += macroQuote(name) + " => {\n"
        if count == 0 {
            val written = if json { tagText + " + \"}\"" } else { prefix + "writeTagged(" + macroQuote(name) + ", None, inline)" }
            encoding += owner + member + " => " + written + ",\n"
            decoding += prefix + "checkFields(value, [\"tag\"], path)?\nOk(" + typeName + member + ")\n},\n"
        } else {
            var slots = ""
            var fields = ""
            decoding += "val payload = " + prefix + "readTaggedValue(value, path)?\nval valuePath = path + \".value\"\n"
            if count > 1 { decoding += "if !payload.isArray() || payload.size() != " + count.toString() + " { return Err(valuePath + \": expected array of " + count.toString() + " items\") }\n" }
            var part = 0
            while part < count {
                val payload = syntaxChild(entry, part + 1)
                val annotation = syntaxChild(payload, syntaxChildCount(payload) - 1)
                val annotationText = syntaxText(annotation)
                val slot = "slot" + part.toString()
                val helper = prefix + target + "Case" + index.toString() + "_" + part.toString()
                val spec = codecFor(syntaxResolvedType(annotation), "", prefix, "tagged", json)
                helpers = syntaxConcat(helpers, syntaxCodeAt(annotation, "fn " + helper + declaration + "(" + parameters + "): " + codecType + "<" + annotationText + "> { " + spec + " }\n"))
                val use = helper + arguments + "(" + codecs + ")"
                if part != 0 { slots += ", "; fields += " + \",\" + " }
                slots += "`" + slot + "`"
                val pathSuffix = if count == 1 { "" } else { " + " + macroQuote("[" + part.toString() + "]") }
                fields += if json { use + ".encode(" + slot + ")" } else { use + ".write(" + slot + ", path + \".value\"" + pathSuffix + ", true)" }
                val data = if count == 1 { "payload" } else { "payload.item(" + part.toString() + ")" }
                decoding += "val " + slot + ": " + annotationText + " = " + use + ".read(" + data + ", valuePath" + pathSuffix + ")?\n"
                part += 1
            }
            val encoded = if count == 1 { fields } else { "\"[\" + " + fields + " + \"]\"" }
            val written = if json { tagText + " + \",\" + " + macroQuote("\"value\":") + " + " + encoded + " + \"}\"" }
                else { prefix + "writeTagged(" + macroQuote(name) + ", Some(" + encoded + "), inline)" }
            encoding += owner + member + "(" + slots + ") => " + written + ",\n"
            decoding += "Ok(" + typeName + member + "(" + slots + "))\n},\n"
        }
        index += 1
    }
    decoding += "`tag` => Err(path + \".tag: unknown " + target + " tag \" + " + prefix + "quote" + format + "(tag)),\n}\n}\n"
    syntaxConcat(helpers, syntaxCode(encoding + "}\n}\n" + decoding + wrappers(target, prefix, format)))
}

/// 在结构体或枚举前写 `#derive(Json)` 或 `#derive(Toml)`，生成对应的静态编解码实现。
fn derive(target: String, argument: String): Syntax {
    if argument != "Json" && argument != "Toml" { return syntaxError(syntaxTarget(), "#derive expects Json or Toml") }
    if targetKind() != "struct" && targetKind() != "enum" { return syntaxError(syntaxTarget(), "#derive(" + argument + ") expects a struct or enum") }
    var names = syntaxText(syntaxTarget())
    var index = 0
    while index < fieldCount() {
        val attribute = if argument == "Json" { "jsonCodec" } else { "tomlCodec" }
        if fieldHasAttribute(index, attribute) { names += "\0" + fieldAttributeValue(index, attribute) }
        index += 1
    }
    var prefix = "__lyra" + argument
    while macroContains(names, prefix) { prefix += "_" }
    if targetKind() == "enum" { deriveEnum(target, prefix, argument) } else { deriveStruct(target, prefix, argument) }
}
