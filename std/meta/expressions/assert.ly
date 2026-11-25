fn assert(target: String, argument: String): Syntax {
    val count = argumentCount()
    if count < 1 || count > 2 { return syntaxError(syntaxTarget(), "#assert expects a condition and optional message") }
    val condition = syntaxArgument(0)
    val message = if count == 2 { syntaxArgument(1) } else { syntaxString("expected condition to be true") }
    quote {
        if splice(condition) {} else { throw Exception("assertion failed: " + splice(message)) }
    }
}
