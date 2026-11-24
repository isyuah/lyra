fn trace(target: String, argument: String): Syntax {
    syntaxAround(syntaxCall(syntaxName("print"), syntaxString("trace: " + target)), syntaxCode(""), syntaxCode(""))
}
