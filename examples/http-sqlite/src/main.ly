import users
import users.{newUserCodec}
import std.cli
import std.http
import std.httpClient
import std.httpTls
import std.io
import std.json
import std.log
import std.sqlite
import std.text.{StringOps}

fn text(code: Int, body: String): http.Response { http.Response.text(code, body) }

fn databaseFailure(logger: log.Logger, route: String, error: sqlite.DbError): http.Response {
    val fields = json.jsonObject().put("route", json.jsonString(route))
        .put("operation", json.jsonString(error.operation)).put("code", json.jsonInt(error.code)).build()
    val _ = logger.error("database error", fields: fields)
    text(500, "database error")
}

async fn getUser(request: http.Request, path: String, logger: log.Logger): http.Response {
    val id = match request.pathInt("id") {
        Ok(Some(value)) => value,
        _ => return text(400, "invalid id"),
    }
    val result = await blocking { users.findUser(path, id) }
    match result {
        Ok(Some(user)) => http.Response.json(user, users.userCodec()),
        Ok(None) => text(404, "not found"),
        Err(error) => databaseFailure(logger, "GET /users/{id}", error),
    }
}

async fn postUser(request: http.Request, path: String, logger: log.Logger): http.Response {
    val input = match request.json(newUserCodec()) {
        Ok(value) => value,
        Err(error) => return text(400, error.message()),
    }
    val result = await blocking { users.insertUser(path, input) }
    match result {
        Ok(user) => http.Response.json(user, users.userCodec(), code: 201),
        Err(error) => databaseFailure(logger, "POST /users", error),
    }
}

async fn getUpstream(url: String, options: httpClient.HttpClientOptions, logger: log.Logger): http.Response {
    match await httpClient.httpGet(url, options) {
        Ok(response) => http.Response.bytes(response.status, "application/octet-stream", response.body),
        Err(error) => {
            val _ = logger.error(error.message(), fields: json.jsonObject().put("route", json.jsonString("GET /upstream")).build())
            text(502, "upstream unavailable")
        },
    }
}

async fn barrier(request: http.Request): http.Response {
    print("barrier")
    http.Response.text(200, request.target())
}

#post("/teams/{teamId}")
@trace
async fn createUser([path] teamId: Int, [query("tag")] tags: List<String>, [query("single")] label: String, [body("newUserCodec")] input: users.NewUser): http.Response {
    http.Response.json(users.CreatedUser(teamId, tags, label, input.name), users.createdUserCodec())
}

fn usageError(message: String): Int {
    val _ = io.writeError(message + "\n")
    2
}

fn pem(path: String): Bytes {
    match io.readBytes(path) { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

async fn main(): Int {
    val spec = cli.Command("users").option("--host").option("--port")
        .option("--cert").option("--key").option("--upstream").option("--ca")
    val arguments = match cli.parseArgs(spec) {
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
        Some(file) => httpTls.listenTls(port, pem(file), pem(key ?? ""), host: host),
        None => http.listen(port, host: host),
    }
    val listener = match started {
        Ok(value) => value,
        Err(error) => throw Exception(error.message()),
    }
    defer listener.close()
    val logger = log.Logger("users", format: log.LogFormat.Json)
    val router = http.Router()
    router.get("/users/{id}", (request: http.Request) => { getUser(request, path, logger) })
    router.post("/users", (request: http.Request) => { postUser(request, path, logger) })
    router.get("/barrier", (request: http.Request) => { barrier(request) })
    if let Some(url) = arguments.value("--upstream") {
        val ca = match arguments.value("--ca") { Some(file) => Some(pem(file)), None => None }
        val options = httpClient.HttpClientOptions(caPem: ca)
        router.get("/upstream", (request: http.Request) => { getUpstream(url, options, logger) })
    }
    createUserRoute(router)
    print(listener.port())
    await router.serve(listener)
    0
}
