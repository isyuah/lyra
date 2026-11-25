import users.CreatedUser
import users.NewUser
import users.createdUserCodec
import users.findUser
import users.insertUser
import users.newUserCodec
import users.userCodec
import std.cli.Command
import std.cli.parseArgs
import std.http.listen
import std.http.Request
import std.http.Response
import std.http.Router
import std.httpClient.HttpClientOptions
import std.httpClient.httpGet
import std.httpTls.listenTls
import std.io.readBytes
import std.io.writeError
import std.json.jsonInt
import std.json.jsonObject
import std.json.jsonString
import std.log.LogFormat
import std.log.Logger
import std.sqlite.DbError
import std.text.StringOps

fn text(code: Int, body: String): Response { Response.text(code, body) }

fn databaseFailure(logger: Logger, route: String, error: DbError): Response {
    val fields = jsonObject().put("route", jsonString(route))
        .put("operation", jsonString(error.operation)).put("code", jsonInt(error.code)).build()
    val _ = logger.error("database error", fields: fields)
    text(500, "database error")
}

async fn getUser(request: Request, path: String, logger: Logger): Response {
    val id = match request.pathInt("id") {
        Ok(Some(value)) => value,
        _ => return text(400, "invalid id"),
    }
    val result = await blocking { findUser(path, id) }
    match result {
        Ok(Some(user)) => Response.json(user, userCodec()),
        Ok(None) => text(404, "not found"),
        Err(error) => databaseFailure(logger, "GET /users/{id}", error),
    }
}

async fn postUser(request: Request, path: String, logger: Logger): Response {
    val input = match request.json(newUserCodec()) {
        Ok(value) => value,
        Err(error) => return text(400, error.message()),
    }
    val result = await blocking { insertUser(path, input) }
    match result {
        Ok(user) => Response.json(user, userCodec(), code: 201),
        Err(error) => databaseFailure(logger, "POST /users", error),
    }
}

async fn getUpstream(url: String, options: HttpClientOptions, logger: Logger): Response {
    match await httpGet(url, options) {
        Ok(response) => Response.bytes(response.status, "application/octet-stream", response.body),
        Err(error) => {
            val _ = logger.error(error.message(), fields: jsonObject().put("route", jsonString("GET /upstream")).build())
            text(502, "upstream unavailable")
        },
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

fn usageError(message: String): Int {
    val _ = writeError(message + "\n")
    2
}

fn pem(path: String): Bytes {
    match readBytes(path) { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

async fn main(): Int {
    val spec = Command("users").option("--host").option("--port")
        .option("--cert").option("--key").option("--upstream").option("--ca")
    val arguments = match parseArgs(spec) {
        Ok(Run(command)) => command,
        Ok(Help(usage)) => { print(usage); return 0 },
        Err(problem) => return usageError(problem.message()),
    }
    if arguments.positionals.size != 1 { return usageError("expected a SQLite database path") }
    val path = arguments.positionals[0]
    val host = arguments.value("--host") ?? "127.0.0.1"
    val port = match (arguments.value("--port") ?? "0").parseInt() {
        Ok(number) => number,
        Err(_) => return usageError("--port must be an integer"),
    }
    val cert = arguments.value("--cert")
    val key = arguments.value("--key")
    if (cert == None) != (key == None) { return usageError("--cert and --key must be supplied together") }
    val started = match cert {
        Some(file) => listenTls(port, pem(file), pem(key ?? ""), host: host),
        None => listen(port, host: host),
    }
    val listener = match started {
        Ok(value) => value,
        Err(error) => throw Exception(error.message()),
    }
    defer listener.close()
    val logger = Logger("users", format: LogFormat.Json)
    val router = Router()
    router.get("/users/{id}", (request: Request) => { getUser(request, path, logger) })
    router.post("/users", (request: Request) => { postUser(request, path, logger) })
    router.get("/barrier", (request: Request) => { barrier(request) })
    if let Some(url) = arguments.value("--upstream") {
        val ca = match arguments.value("--ca") { Some(file) => Some(pem(file)), None => None }
        val options = HttpClientOptions(caPem: ca)
        router.get("/upstream", (request: Request) => { getUpstream(url, options, logger) })
    }
    createUserRoute(router)
    print(listener.port())
    await router.serve(listener)
    0
}
