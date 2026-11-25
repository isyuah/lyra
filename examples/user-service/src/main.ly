import std.http.{HttpError, Request, Response, Server}
import std.json.{Json}
import std.log.{reportError}
import std.task.{blocking}
import sqlite.{Connection, DbError}

#derive(Json)
struct User {
    pub id: Int
    pub name: String
}

implicit (user: User): Response { Response.json(user) }

pub async fn main(): Result<Unit, HttpError> {
    val server = Server().get("/users/{id}", getUser)
    await server.serve(host: "127.0.0.1", port: 8080)
}

async fn getUser(request: Request): Response {
    val id = request.pathInt("id")
    if id == null { return Response.status(400) }

    val result = await findUser(id)
    match result {
        Ok(Some(user)) => user,
        Ok(None) => Response.status(404),
        Err(error) => {
            reportError(error)
            Response.status(500)
        },
    }
}

async fn findUser(id: Int): Result<User?, DbError> {
    await blocking {
        val db = Connection.openReadOnly("users.db")?
        defer db.close()
        val query = db.prepare("SELECT id, name FROM users WHERE id = ?1")?
        defer query.close()
        query.bindInt(1, id)?

        if query.next()? {
            val user = User(id: query.int(0)?, name: query.text(1)?)
            Ok(Some(user))
        } else {
            Ok(None)
        }
    }
}
