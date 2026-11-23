import std.http.listen
import std.http.Request
import std.io.arg
import std.io.assert
import std.io.readLine
import std.sqlite.openReadOnly

async fn observe(target: Task<Unit>): Unit { await target }
async fn group(child: Task<Unit>): Unit { spawn child; }

async fn handle(request: Request, work: Task<Unit>, probe: Task<Unit>): Unit {
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
        val db = openReadOnly(path)
        val query = db.prepare("SELECT id FROM rows ORDER BY id")
        assert(query.next().row() && query.readInt(0).value == 7, "first row")
        print("cursor open")
        assert(readLine().value == "continue", "release worker")
        assert(query.next().row() && query.readInt(0).value == 8 && query.next().done(), "cursor still usable")
        assert(query.close().ok() && db.close().ok(), "close cursor and database")
        print("cursor closed")
    }
    val probe = observe(work)
    val serving = spawn group(handle(request, work, probe))
    await group(observe(probe))
    print("cancellation observed")
    await serving
    request.close()
    print("request group closed")

    val next = await blocking { server.accept(5000) }
    assert(next.status() == 0 && next.respond(200, "text/plain", "ready") == 0, "serve next request")
    server.close()
}
