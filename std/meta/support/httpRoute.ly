fn httpRoute(target: String, route: String, method: String): Syntax {
    val declaration = syntaxTarget()
    var bodyCount = 0
    var bodyType = ""
    var bodyCodec = ""
    var arguments = ""
    var factoryParameters = ""
    var adapterParameters = ""
    var adapterArguments = ""
    var captures = ""
    var setup = ""
    var index = 0
    while index < fieldCount() {
        val name = fieldName(index)
        val parameter = syntaxChild(declaration, index + 1)
        val annotation = syntaxChild(parameter, 1)
        val path = fieldHasAttribute(index, "path")
        val query = fieldHasAttribute(index, "query")
        val body = fieldHasAttribute(index, "body")
        if path {
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
        } else if fieldHasAttribute(index, "inject") {
            val dependency = "__lyraPostValue" + index.toString()
            val input = "__lyraHttpInput" + index.toString()
            if factoryParameters != "" { factoryParameters += ", " }
            factoryParameters += "in " + input + ": " + syntaxText(annotation)
            adapterParameters += ", " + dependency + ": " + syntaxText(annotation)
            adapterArguments += ", " + dependency
            captures += "val " + dependency + " = " + input + "\n"
            if arguments != "" { arguments += ", " }
            arguments += dependency
        } else {
            return syntaxError(syntaxChild(parameter, 2), "unsupported HTTP parameter attribute")
        }
        index += 1
    }
    if bodyCount == 1 {
        if bodyCodec == "" { bodyCodec = syntaxText(syntaxName("jsonCodec" + bodyType)) }
        setup += "match request.json(" + bodyCodec + "()) { Ok(__lyraPostBody) => await `" + target + "`(" + arguments + "), Err(_) => __lyraPostHttp.Response.text(400, \"invalid JSON body\") }"
    } else {
        setup += "await `" + target + "`(" + arguments + ")"
    }
    val registration = "fn __lyraHttpFactory(" + factoryParameters + "): __lyraPostHttp.RouteRegistration {\n" + captures +
        "__lyraPostHttp.RouteRegistration(" + macroQuote(method) + ", " + macroQuote(route) + ", (request: __lyraPostHttp.Request) => { __lyraHttpAdapter(request" + adapterArguments + ") }, " + macroQuote(httpRouteOrigin()) + ")\n}\n"
    val adapter = "async fn __lyraHttpAdapter(request: __lyraPostHttp.Request" + adapterParameters + "): __lyraPostHttp.Response {\n" + setup + "\n}\n"
    syntaxCode("import std.http as __lyraPostHttp\n" + adapter + registration)
}
