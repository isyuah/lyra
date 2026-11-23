fn userQuery(target: String, argument: String): String {
    if argumentCount() != 3 { return macroFail("expected database, SQL literal and user id") }
    val sql = argumentString(1)
    val signature = "lyra-sqlite-example/1\nparameter: Int; column: name String\n" + sql + "\n-- schema --\n" + macroInput("schema")
    if signature != macroInput("snapshot") { return macroFail("userById snapshot mismatch: check tools/userById.sql and run tools/prepareQuery.py") }
    "prepareUserById(" + argumentCode(0) + ", " + macroQuote(sql) + ", " + argumentCode(2) + ")"
}
