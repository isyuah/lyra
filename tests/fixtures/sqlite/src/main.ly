import std.io.assert
import std.sqlite.open

fn main(): Unit {
    val db = open(":memory:")
    val query = db.prepare("SELECT ?1")
    assert(query.bindText(1, "SQLite: 文本").ok() && query.next().row(), "query")
    val read = query.readText(0)
    assert(read.status.ok() && query.close().ok() && db.close().ok(), "close")
    gcCollect()
    print(read.value)
}
