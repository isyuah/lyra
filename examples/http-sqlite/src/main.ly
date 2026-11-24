import users.CreatedUser
import users.NewUser
import users.createdUserCodec
import users.findUser
import users.newUserCodec
import users.userCodec
import std.http.listen
import std.http.Request
import std.http.Response
import std.http.Router
import std.io.arg
import std.io.assert
import std.io.writeError

fn text(code: Int, body: String): Response { Response.text(code, body) }

async fn getUser(request: Request, path: String): Response {
    val id = match request.pathInt("id") {
        Ok(Some(value)) => value,
        _ => return text(400, "invalid id"),
    }
    val result = await blocking { findUser(path, id) }
    match result {
        Ok(Some(user)) => Response.json(user, userCodec()),
        Ok(None) => text(404, "not found"),
        Err(error) => { writeError("GET ${request.target()}: ${error.operation}: ${error.message()}\n"); text(500, "database error") },
    }
}

async fn barrier(request: Request): Response {
    print("barrier")
    Response.text(200, request.target())
}

#post("/teams/{teamId}")
@trace
async fn createUser([path] teamId: Int, [query("tag")] tags: List<String>, [query("single")] label: String, [body("newUserCodec")] input: NewUser): Response {
    Response.json(CreatedUser(teamId, tags, label, input.name), createdUserCodec())
}

async fn main(): Unit {
    val path = arg(0)
    assert(path.ok(), "expected a SQLite database path")
    val listener = listen(0)
    assert(listener.status() == 0, "listen failed")
    val router = Router()
    router.get("/users/{id}", (request: Request) => { getUser(request, path.value) })
    router.get("/barrier", (request: Request) => { barrier(request) })
    createUserRoute(router)
    print(listener.port())
    await router.serve(listener)
}
