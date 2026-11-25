import std.sqlite.{DbError, open}

fn read(): Result<String?, DbError> {
    val db = open(":memory:")?
    defer db.close()
    val query = db.prepare("SELECT ?1")?
    defer query.close()
    query.bindText(1, "SQLite: 文本")?
    #assert(query.next()?, "query")
    query.readText(0)
}

fn main(): Unit {
    val result = read()
    gcCollect()
    match result {
        Ok(Some(text)) => print(text),
        Ok(None) => throw Exception("missing text"),
        Err(error) => throw Exception(error.message()),
    }
}
