fn supportedAttribute(index: Int): String {
    val declaration = syntaxTarget()
    val parameter = syntaxChild(declaration, index + 1)
    var child = 2
    while child < syntaxChildCount(parameter) {
        val attribute = syntaxChild(parameter, child)
        val name = syntaxText(attribute)
        if name != "path" && name != "query" && name != "body" {
            return name
        }
        child += 1
    }
    ""
}

fn post(target: String, route: String): Syntax {
    val declaration = syntaxTarget()
    if syntaxKind(declaration) != "fn" { return syntaxError(declaration, "#post can only decorate a function") }
    var bodyCount = 0
    var bodyType = ""
    var bodyCodec = ""
    var pathNames = "\0"
    var arguments = ""
    var mountParameters = ""
    var mountArguments = ""
    var setup = ""
    var index = 0
    while index < fieldCount() {
        val name = fieldName(index)
        val parameter = syntaxChild(declaration, index + 1)
        val annotation = syntaxChild(parameter, 1)
        val path = fieldHasAttribute(index, "path")
        val query = fieldHasAttribute(index, "query")
        val body = fieldHasAttribute(index, "body")
        val attribute = supportedAttribute(index)
        if attribute != "" { return syntaxError(syntaxChild(parameter, 2), "unsupported HTTP parameter attribute: " + attribute) }
        if path && query || path && body || query && body { return syntaxError(parameter, "an HTTP parameter can have only one source attribute") }
        if path {
            if fieldAttributeValue(index, "path") != "" { return syntaxError(parameter, "[path] does not accept an argument") }
            if !macroContains(route, "{" + name + "}") { return syntaxError(parameter, "path parameter is absent from the route: " + name) }
            if macroContains(pathNames, "\0" + name + "\0") { return syntaxError(parameter, "duplicate HTTP path parameter: " + name) }
            pathNames += name + "\0"
            val parameterType = syntaxResolvedType(annotation)
            val typeName = syntaxText(parameterType)
            if syntaxKind(parameterType) == "builtin" && typeName == "Int" {
                setup += "val __lyraPostValue" + index.toString() + ": Int = match request.pathInt(" + macroQuote(name) + ") { Ok(Some(value)) => value, _ => { return __lyraPostHttp.Response.text(400, " + macroQuote("invalid path parameter: " + name) + ") } }\n"
            } else if syntaxKind(parameterType) == "builtin" && typeName == "String" {
                setup += "val __lyraPostValue" + index.toString() + ": String = match request.path(" + macroQuote(name) + ") { Some(value) => value, None => { return __lyraPostHttp.Response.text(400, " + macroQuote("invalid path parameter: " + name) + ") } }\n"
            } else { return syntaxError(parameter, "[path] supports Int and String parameters") }
            if arguments != "" { arguments += ", " }
            arguments += "__lyraPostValue" + index.toString()
        } else if query {
            var key = fieldAttributeValue(index, "query")
            if key == "" { key = name }
            val parameterType = syntaxResolvedType(annotation)
            val typeName = syntaxText(parameterType)
            if syntaxKind(parameterType) == "builtin" && typeName == "List" {
                val element = syntaxChild(parameterType, 0)
                if syntaxKind(element) != "builtin" || syntaxText(element) != "String" { return syntaxError(parameter, "[query] supports String and List<String> parameters") }
                setup += "val __lyraPostValue" + index.toString() + " = request.queryAll(" + macroQuote(key) + ")\n"
            } else if syntaxKind(parameterType) == "builtin" && typeName == "String" {
                setup += "val __lyraPostValues" + index.toString() + " = request.queryAll(" + macroQuote(key) + ")\n"
                setup += "if __lyraPostValues" + index.toString() + ".size != 1 { return __lyraPostHttp.Response.text(400, " + macroQuote("invalid query parameter: " + key) + ") }\n"
                setup += "val __lyraPostValue" + index.toString() + " = __lyraPostValues" + index.toString() + "[0]\n"
            } else { return syntaxError(parameter, "[query] supports String and List<String> parameters") }
            if arguments != "" { arguments += ", " }
            arguments += "__lyraPostValue" + index.toString()
        } else if body {
            bodyCount += 1
            if bodyCount > 1 { return syntaxError(parameter, "an HTTP handler can have only one [body] parameter") }
            bodyCodec = fieldAttributeValue(index, "body")
            if bodyCodec != "" { bodyCodec = syntaxText(syntaxName(bodyCodec)) }
            val parameterType = syntaxResolvedType(annotation)
            val kind = syntaxKind(parameterType)
            if (kind != "named" && kind != "standard") || syntaxChildCount(parameterType) != 0 { return syntaxError(parameter, "[body] requires a named type with a JSON codec") }
            bodyType = syntaxText(parameterType)
            if arguments != "" { arguments += ", " }
            arguments += "__lyraPostBody"
        } else {
            if mountParameters != "" { mountParameters += ", " }
            if mountArguments != "" { mountArguments += ", " }
            mountParameters += "`" + name + "`: " + syntaxText(annotation)
            mountArguments += "`" + name + "`"
            if arguments != "" { arguments += ", " }
            arguments += "`" + name + "`"
        }
        index += 1
    }
    if bodyCount == 1 {
        if bodyCodec == "" { bodyCodec = syntaxText(syntaxName("jsonCodec" + bodyType)) }
        setup += "match request.json(" + bodyCodec + "()) { Ok(__lyraPostBody) => await `" + target + "`(" + arguments + "), Err(_) => __lyraPostHttp.Response.text(400, \"invalid JSON body\") }"
    } else {
        setup += "await `" + target + "`(" + arguments + ")"
    }
    val mountSeparator = if mountParameters == "" { "" } else { ", " + mountParameters }
    val callSeparator = if mountArguments == "" { "" } else { ", " + mountArguments }
    val handler = "__lyraPostHandler_" + target
    val registration = "fn `" + target + "Route`(router: __lyraPostHttp.Router" + mountSeparator + "): Unit { router.route(\"POST\", " + macroQuote(route) + ", (request: __lyraPostHttp.Request) => { `" + handler + "`(request" + callSeparator + ") }) }\n"
    val adapter = "async fn `" + handler + "`(request: __lyraPostHttp.Request" + mountSeparator + "): __lyraPostHttp.Response {\n" + setup + "\n}\n"
    syntaxCode("import std.http as __lyraPostHttp\n" + adapter + registration)
}
