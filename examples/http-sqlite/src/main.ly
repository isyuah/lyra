import std.http.listen
import std.http.Request
import std.http.jsonString
import std.io.arg
import std.io.assert
import std.sqlite.openReadOnly

fn lookup(request: Request, path: String): Unit {
    val id = request.pathInt("/users/", -1)
    if id < 0 { request.respond(400, "text/plain", "invalid id"); return }
    val db = openReadOnly(path)
    if !db.status().ok() { request.respond(500, "text/plain", "database unavailable"); return }
    val query = db.prepare("SELECT name FROM users WHERE id=?1")
    var code = 500
    var body = "database error"
    if query.status().ok() && query.bindInt(1, id).ok() && !request.disconnected() {
        val cursor = query.next()
        if cursor.row() {
            val name = query.readText(0)
            if name.status.ok() { body = "{\"name\":" + jsonString(name.value) + "}"; code = 200 }
        } else if cursor.done() { code = 404; body = "not found" }
    }
    if query.status().ok() { query.close(); }
    db.close()
    if request.disconnected() { request.close(); return }
    request.respond(code, if code == 200 { "application/json" } else { "text/plain" }, body);
}

fn main(): Unit {
    val path = arg(0)
    assert(path.ok(), "expected a SQLite database path")
    val server = listen(0)
    assert(server.status() == 0, "listen failed")
    print(server.port())
    while true {
        val request = server.accept(30000)
        if request.status() == 0 {
            if request.method() == "GET" { lookup(request, path.value) }
            else { request.respond(405, "text/plain", "method not allowed"); }
        }
    }
}
