/// Mount this package's annotated routes as one batch, optionally with dependencies.
fn mountRoutes(target: String, argument: String): Syntax {
    val count = argumentCount()
    if count < 1 || count > 2 { val _ = macroFail("#mountRoutes expects a Router and optional dependencies") }
    val router = syntaxArgument(0)
    val arguments = if count == 2 { syntaxArgs(router, syntaxArgument(1)) } else { router }
    syntaxCall(syntaxName("__lyraHttpMountRoutes"), arguments)
}
