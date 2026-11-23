import std.http.listen
import std.http.Request
import std.io.arg
import std.io.assert
import std.io.readLine
import std.sqlite.DbError
import std.sqlite.Statement
import std.sqlite.openReadOnly

fn require<T>(result: Result<T, DbError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

fn currentId(query: Statement): Int {
    match require(query.readInt(0)) { Some(id) => id, None => throw Exception("missing id") }
}

async fn observe(target: Task<Unit>): Unit { await target }
async fn group(child: Task<Unit>): Unit { spawn child; }

async fn handle(request: Request, work: Task<Unit>, probe: Task<Unit>): Unit {
    defer { request.close(); print("request closed") }
    assert(request.watchDisconnect(), "watch owner disconnect")
    spawn work;
    spawn probe;
    await work
    print("handler resumed")
    request.respond(200, "text/plain", "done");
}

async fn main(): Unit {
    val server = listen(0)
    assert(server.status() == 0, "listen")
    print(server.port())
    val request = await blocking { server.accept(5000) }
    assert(request.status() == 0, "accept")
    val path = arg(0).value
    val work = blocking {
        val db = require(openReadOnly(path))
        defer { db.close(); print("database closed") }
        val query = require(db.prepare("SELECT id FROM rows ORDER BY id"))
        defer { query.close(); print("cursor closed") }
        assert(require(query.next()) && currentId(query) == 7, "first row")
        print("cursor open")
        assert(readLine().value == "continue", "release worker")
        assert(require(query.next()) && currentId(query) == 8 && !require(query.next()), "cursor still usable")
    }
    val probe = observe(work)
    val serving = spawn group(handle(request, work, probe))
    await group(observe(probe))
    print("cancellation observed")
    await serving
    print("request group closed")

    val next = await blocking { server.accept(5000) }
    assert(next.status() == 0 && next.respond(200, "text/plain", "ready") == 0, "serve next request")
    server.close()
}
