import std.json.JsonCodec
import std.sqlite.Connection
import std.sqlite.DbError
import std.sqlite.Statement
import std.sqlite.openReadOnly

#derive(Json)
pub struct User(pub val id: Int, pub val name: String)

#derive(Json)
pub struct NewUser(pub val name: String)

#derive(Json)
pub struct CreatedUser(pub val teamId: Int, pub val tags: List<String>, pub val label: String, pub val name: String)

pub fn userCodec(): JsonCodec<User> { jsonCodecUser() }
pub fn newUserCodec(): JsonCodec<NewUser> { jsonCodecNewUser() }
pub fn createdUserCodec(): JsonCodec<CreatedUser> { jsonCodecCreatedUser() }

fn prepareUserById(db: Connection, sql: String, id: Int): Result<Statement, DbError> {
    val query = db.prepare(sql)?
    match query.bindInt(1, id) {
        Ok(_) => Ok(query),
        Err(error) => { query.close(); Err(error) },
    }
}

pub fn findUser(path: String, id: Int): Result<User?, DbError> {
    val db = openReadOnly(path)?
    defer db.close()
    val query = #userQuery(db, "SELECT name FROM users WHERE id=?1", id)?
    defer query.close()
    if !query.next()? { return Ok(None) }
    match query.readText(0)? {
        Some(name) => Ok(Some(User(id, name))),
        None => Err(DbError.typeMismatch("readText", "users.name must not be NULL")),
    }
}
