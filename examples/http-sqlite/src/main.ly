import std.http.listen
import std.http.Request
import std.http.Response
import std.io.arg
import std.io.assert
import std.io.writeError
import std.sqlite.Connection
import std.sqlite.DbStatus
import std.sqlite.Statement
import std.sqlite.openReadOnly

#derive(Json)
struct User(pub val id: Int, pub val name: String)

implicit (user: User): Response { Response(200, "application/json", encodeUser(user)) }

fn text(code: Int, body: String): Response { Response(code, "text/plain", body) }
fn cause(status: DbStatus): String { status.operation() + ": " + status.message() }

fn prepareUserById(db: Connection, sql: String, id: Int): Result<Statement,String> {
    val query = db.prepare(sql)
    if !query.status().ok() { return Err(cause(query.status())) }
    val bound = query.bindInt(1, id)
    if !bound.ok() {
        val reason = cause(bound)
        query.close()
        return Err(reason)
    }
    Ok(query)
}

fn readUser(query: Statement, id: Int): Result<User?,String> {
    val cursor = query.next()
    if cursor.done() { return Ok(None) }
    if !cursor.row() { return Err(cause(cursor)) }
    val name = query.readText(0)
    if !name.status.ok() { return Err(cause(name.status)) }
    Ok(Some(User(id, name.value)))
}

fn lookup(path: String, id: Int): Result<User?,String> {
    val db = openReadOnly(path)
    if !db.status().ok() { return Err(cause(db.status())) }
    val query = match #userQuery(db, "SELECT name FROM users WHERE id=?1", id) {
        Ok(cursor) => cursor,
        Err(reason) => { db.close(); return Err(reason) },
    }
    val result = readUser(query, id)
    val cursorClosed = query.close()
    val dbClosed = db.close()
    if !cursorClosed.ok() { return Err(cause(cursorClosed)) }
    if !dbClosed.ok() { return Err(cause(dbClosed)) }
    result
}

async fn route(request: Request, path: String): Response {
    if request.method() != "GET" { return text(405, "method not allowed") }
    val id = request.pathInt("/users/", -1)
    if id < 0 { return text(400, "invalid id") }
    val result = await blocking { lookup(path, id) }
    match result {
        Ok(Some(user)) => user,
        Ok(None) => text(404, "not found"),
        Err(message) => { writeError("GET " + request.target() + ": " + message + "\n"); text(500, "database error") },
    }
}

async fn handle(request: Request, path: String): Unit {
    defer request.close()
    val response = if request.watchDisconnect() { await route(request, path) } else { text(500, "disconnect watch unavailable") }
    if !request.disconnected() {
        await blocking { request.respond(response.code, response.contentType, response.body); };
    }
}

async fn requestGroup(request: Request, path: String): Unit { spawn handle(request, path); }

async fn main(): Unit {
    val path = arg(0)
    assert(path.ok(), "expected a SQLite database path")
    val server = listen(0)
    assert(server.status() == 0, "listen failed")
    print(server.port())
    while true {
        val request = await blocking { server.accept(30000) }
        if request.status() == 0 {
            await requestGroup(request, path.value)
        }
    }
}
