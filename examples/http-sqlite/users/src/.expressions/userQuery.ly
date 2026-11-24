fn userQuery(target: String, argument: String): Syntax {
    if argumentCount() != 3 { return syntaxError(syntaxCode(""), "expected database, SQL literal and user id") }
    val sql = argumentString(1)
    val signature = "lyra-sqlite-example/1\nparameter: Int; column: name String\n" + sql + "\n-- schema --\n" + macroInput("schema")
    if signature != macroInput("snapshot") { return syntaxError(syntaxArgument(1), "userById snapshot mismatch: check tools/userById.sql and run tools/prepareQuery.py") }
    syntaxCall(syntaxName("prepareUserById"), syntaxArgs(syntaxArgument(0), syntaxArgs(syntaxString(sql), syntaxArgument(2))))
}
