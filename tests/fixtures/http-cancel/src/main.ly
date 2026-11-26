import std.http.{listen, HttpError, Request, Response}
import std.io.{IoError, readLine}
import std.os.{args}
import std.sqlite.{DbError, Statement, openReadOnly}

fn require<T>(result: Result<T, DbError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

fn requireHttp<T>(result: Result<T, HttpError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

fn requireIo<T>(result: Result<T, IoError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

fn currentId(query: Statement): Int {
    match require(query.readInt(0)) { Some(id) => id, None => throw Exception("missing id") }
}

async fn observe(target: Task<Unit>): Unit { await target }
async fn group(child: Task<Unit>): Unit { spawn child; }

async fn watchDisconnect(request: Request, handler: Task<Unit>): Unit {
    if await request.waitDisconnect() { handler.cancel() }
}

async fn handle(request: Request, work: Task<Unit>, probe: Task<Unit>): Unit {
    spawn work;
    spawn probe;
    await work
    print("handler resumed")
    requireHttp(await request.respond(Response.text(200, "done")))
}

async fn main(): Unit {
    val server = requireHttp(listen(0))
    print(server.port())
    val path = requireIo(args())[0]
    val work = blocking {
        val db = require(openReadOnly(path))
        defer { db.close(); print("database closed") }
        val query = require(db.prepare("SELECT id FROM rows ORDER BY id"))
        defer { query.close(); print("cursor closed") }
        #assert(require(query.next()) && currentId(query) == 7, "first row")
        print("cursor open")
        #assert(requireIo(readLine()) == Some("continue"), "release worker")
        #assert(require(query.next()) && currentId(query) == 8 && !require(query.next()), "cursor still usable")
    }
    val probe = observe(work)
    requireHttp(await server.withAccepted<Unit>(5000, async (request: Request) => {
        taskGroup {
            val serving = spawn handle(request, work, probe)
            val watcher = spawn watchDisconnect(request, serving)
            await group(observe(probe))
            print("cancellation observed")
            val _ = awaitOutcome(serving)
            watcher.cancel()
        };
    }))
    print("request closed")
    print("request group closed")

    requireHttp(await server.withAccepted<Unit>(5000, async (request: Request) => {
        requireHttp(await request.respond(Response.text(200, "ready")))
    }))
    await server.closeAndWait()
}
