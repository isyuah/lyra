import contextlib
import http.client
import json
import os
from pathlib import Path
import queue
import shutil
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import unittest

from platform_cli import WINDOWS, assert_linkage, executable, running


COMPILER = Path(sys.argv.pop(1)).resolve()
SERVICE = Path(__file__).resolve().parents[1] / "examples/http-sqlite"
CANCELLATION = Path(__file__).resolve().parent / "fixtures/http-cancel"

HTTP_HELPERS = '''
import std.http.{HttpError}
import std.http.{HttpErrorKind}
import std.http.{listen}
import std.http.{Response}

fn requireHttp<T>(result: Result<T, HttpError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

fn failed<T>(result: Result<T, HttpError>, expected: HttpErrorKind): Bool {
    match result { Err(error) => error.kind == expected, Ok(_) => false }
}
'''


@contextlib.contextmanager
def server(app, *arguments):
    with running([app, *arguments], env={**os.environ, "UV_THREADPOOL_SIZE": "1"}) as (process, output):
        yield int(output.line()), process, output


def reset_on_close(connection):
    connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('HH' if WINDOWS else 'ii', 1, 0))


def exchange(port, method, target, body=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        conn.request(method, target, body=body)
        response = conn.getresponse()
        return response.status, response.getheader("Content-Type"), response.read()
    finally:
        conn.close()


class HTTP(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / "HTTP 服务"
        self.root.mkdir()

    def profiles(self, package, *package_flags):
        for profile, flags in (("debug", []), ("release", ["--release"])):
            output = executable(self.root / profile)
            built = subprocess.run([COMPILER, "build", package, *package_flags, "-o", output, *flags], capture_output=True, timeout=45)
            self.assertEqual(built.returncode, 0, built.stderr.decode())
            yield profile, output

    def test_sqlite_service(self):
        package = self.root / "service"
        shutil.copytree(SERVICE, package, ignore=shutil.ignore_patterns("build", ".lyra"))
        subprocess.run([COMPILER, 'check', package, '--locked', '--offline'], check=True, capture_output=True, timeout=30)
        for profile, app in self.profiles(package, "--locked", "--offline"):
            with self.subTest(profile=profile):
                database = self.root / (profile + ".db")
                name = '李"\\\n\0Ada'
                with contextlib.closing(sqlite3.connect(database)) as connection, connection:
                    connection.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT)")
                    connection.execute("INSERT INTO users VALUES(7,?)", (name,))
                    connection.execute("INSERT INTO users VALUES(9,?)", ("x" * (2 * 1024 * 1024),))
                with server(app, database) as (port, process, output):
                    with socket.socket() as drop:
                        drop.settimeout(5)
                        drop.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                        drop.connect(("127.0.0.1", port))
                        drop.sendall(b"GET /users/9 HTTP/1.1\r\nHost: localhost\r\n\r\n")
                        self.assertEqual(drop.recv(1), b"H")
                        reset_on_close(drop)
                    status, mime, body = exchange(port, "GET", "/users/7?view=one")
                    self.assertEqual((status, mime, json.loads(body)), (200, "application/json", {"id": 7, "name": name}))
                    self.assertEqual(exchange(port, "GET", "/users/8")[0], 404)
                    self.assertEqual(exchange(port, "GET", "/users/invalid")[0], 400)
                    self.assertEqual(exchange(port, "POST", "/users/7", b"body")[0], 405)
                    status, mime, body = exchange(
                        port, "POST", "/teams/%37?tag=a+b&tag=c%2Bd&single=chosen",
                        json.dumps({"name": "Ada"}).encode(),
                    )
                    self.assertEqual((status, mime, json.loads(body)), (200, "application/json", {
                        "teamId": 7, "tags": ["a b", "c+d"], "label": "chosen", "name": "Ada",
                    }))
                    self.assertEqual(output.line(), b"trace: createUser\n")
                    for target, request_body in (
                        ("/teams/9223372036854775808?single=x", b'{"name":"Ada"}'),
                        ("/teams/7?single=one&single=two", b'{"name":"Ada"}'),
                        ("/teams/7?single=x", b"{"),
                        ("/teams/7?single=x", b""),
                    ):
                        self.assertEqual(exchange(port, "POST", target, request_body)[0], 400)
                    self.assertEqual(exchange(port, "POST", "/teams/%ZZ?single=x", b'{"name":"Ada"}')[0], 400)
                    with self.assertRaises(queue.Empty):
                        output.pending.get_nowait()
                    status, _, body = exchange(
                        port, "POST", "/teams/7?single=x", json.dumps({"name": "Grace"}).encode(),
                    )
                    self.assertEqual((status, json.loads(body)["name"]), (200, "Grace"))
                    self.assertEqual(output.line(), b"trace: createUser\n")
                    database.unlink()
                    self.assertEqual(exchange(port, "GET", "/users/7")[0], 500)
                    with contextlib.closing(sqlite3.connect(database)) as empty:
                        empty.execute("CREATE TABLE unrelated(id INTEGER PRIMARY KEY)")
                    self.assertEqual(exchange(port, "GET", "/users/7")[0], 500)
                    self.assertIsNone(process.poll())
                assert_linkage(self, app, 'libm.so.6')

    def test_users_library_without_http(self):
        library = self.root / 'users'
        shutil.copytree(SERVICE / 'users', library, ignore=shutil.ignore_patterns('build', '.lyra', 'lyra.lock'))
        database = self.root / 'users.db'
        missing_table = self.root / 'empty.db'
        with contextlib.closing(sqlite3.connect(database)) as connection, connection:
            connection.executescript((library / 'inputs/schema.sql').read_text(encoding='utf-8'))
            connection.execute("INSERT INTO users VALUES(7, 'Ada')")
        with contextlib.closing(sqlite3.connect(missing_table)) as connection:
            connection.execute('CREATE TABLE unrelated(id INTEGER PRIMARY KEY)')

        consumer = self.root / 'consumer'
        (consumer / 'src').mkdir(parents=True)
        (consumer / 'lyra.json5').write_text('''{
  name: 'test/user-consumer', version: '1.0.0',
  dependencies: { store: { name: 'example/users', version: '^0.1.0', path: '../users' } },
}''', encoding='utf-8')
        (consumer / 'src/main.ly').write_text(r'''
import store.{findUser}
import store.{newUserCodec}
import store.{userCodec}
import std.os.{args}
import std.sqlite.{DbErrorKind}

fn main(): Unit {
    val arguments = match args() { Ok(values) => values, Err(error) => throw Exception(error.message()) }
    #assert(arguments.size == 2, "expected database paths")
    val database = arguments[0]
    val missingTable = arguments[1]
    #assert(match findUser(database, 7) {
        Ok(Some(user)) => {
            val codec = userCodec()
            match codec.decode(codec.encode(user)) { Ok(decoded) => decoded.id == 7 && decoded.name == "Ada", Err(_) => false }
        },
        _ => false,
    }, "expected user and JSON round trip")
    #assert(match findUser(database, 8) { Ok(None) => true, _ => false }, "no row is not an error")
    #assert(match findUser(missingTable, 7) {
        Err(error) => match error.kind { DbErrorKind.Sqlite => error.operation == "prepare", _ => false },
        Ok(_) => false,
    }, "expected typed database error")
    #assert(match newUserCodec().decode(r#"{"name":"Ada","extra":true}"#) {
        Err(message) => message == "$.extra: unknown field",
        Ok(_) => false,
    }, "expected the library's strict body codec")
}
''', encoding='utf-8')
        locked = subprocess.run([COMPILER, 'lock', consumer, '--offline'], capture_output=True, timeout=20)
        self.assertEqual(locked.returncode, 0, locked.stderr.decode())
        run = subprocess.run([COMPILER, 'run', consumer, '--locked', '--offline', '--', database, missing_table],
                             capture_output=True, timeout=45)
        self.assertEqual(run.returncode, 0, run.stderr.decode())

    def test_router_capacity_releases_after_disconnect(self):
        package = self.root / "capacity"
        (package / "src").mkdir(parents=True)
        (package / "lyra.json5").write_text("{name:'test/http-capacity',version:'1.0.0'}")
        (package / "src/main.ly").write_text(HTTP_HELPERS + '''
import std.http.{Request}
import std.http.{Router}
async fn hold(request: Request): Response {
    print("entered")
    await blocking { request.waitDisconnect(15000); }
    Response.text(200, "released")
}
async fn ready(request: Request): Response { Response.text(200, "ready") }
async fn main(): Unit {
    val listener = requireHttp(listen(0))
    val router = Router()
    router.get("/hold", (request: Request) => { hold(request) })
    router.get("/ready", (request: Request) => { ready(request) })
    print(listener.port())
    await router.serve(listener, 1)
}
''')
        for profile, app in self.profiles(package):
            with self.subTest(profile=profile), server(app) as (port, process, output):
                first = socket.create_connection(("127.0.0.1", port), timeout=5)
                self.addCleanup(first.close)
                first.sendall(b"GET /hold HTTP/1.1\r\nHost: localhost\r\n\r\n")
                self.assertEqual(output.line(), b"entered\n")
                second = socket.create_connection(("127.0.0.1", port), timeout=5)
                self.addCleanup(second.close)
                second.sendall(b"GET /ready HTTP/1.1\r\nHost: localhost\r\n\r\n")
                second.settimeout(0.1)
                with self.assertRaises(socket.timeout):
                    second.recv(1)
                reset_on_close(first)
                first.close()
                second.settimeout(5)
                response = http.client.HTTPResponse(second)
                response.begin()
                self.assertEqual((response.status, response.read()), (200, b"ready"))

    def test_handler_bounds_failure_does_not_stop_the_server(self):
        package = self.root / "handler-error"
        (package / "src").mkdir(parents=True)
        (package / "lyra.json5").write_text("{name:'test/handler-error',version:'1.0.0'}")
        (package / "src/main.ly").write_text(HTTP_HELPERS + '''
import std.http.{Request}
import std.http.{Router}
async fn main(): Unit {
    val listener = requireHttp(listen(0))
    val router = Router()
    router.get("/fail", async (request: Request) => {
        defer print("handler cleanup")
        Response.text(200, ["one"][1])
    })
    router.get("/ready", async (request: Request) => { Response.text(200, "ready") })
    print(listener.port())
    await router.serve(listener, 1)
}
''', encoding="utf-8")
        for profile, app in self.profiles(package):
            with self.subTest(profile=profile), server(app) as (port, _, output):
                self.assertEqual(exchange(port, "GET", "/fail")[0], 500)
                self.assertEqual(output.line().strip(), b"handler cleanup")
                self.assertEqual(exchange(port, "GET", "/ready"), (200, "text/plain", b"ready"))

    def test_bind_methods_head_and_allow(self):
        package = self.root / "methods"
        (package / "src").mkdir(parents=True)
        (package / "lyra.json5").write_text("{name:'test/http-methods',version:'1.0.0'}")
        (package / "src/main.ly").write_text(HTTP_HELPERS + '''
import std.http.{Request}
import std.http.{Router}

async fn main(): Unit {
    #assert(failed(listen(-1), HttpErrorKind.Invalid), "invalid port")
    #assert(failed(listen(0, host: "not an address"), HttpErrorKind.Invalid), "invalid address")
    val listener = requireHttp(listen(0, host: "0.0.0.0"))
    #assert(match listen(listener.port(), host: "0.0.0.0") {
        Err(error) => match error.kind { HttpErrorKind.System(_) => true, _ => false },
        Ok(other) => { other.close(); false },
    }, "bind conflict")
    val router = Router()
    router.get("/fallback", async (request: Request) => { Response.text(200, "body") })
    router.get("/item", async (request: Request) => { Response.text(200, "get") })
    router.head("/item", async (request: Request) => { Response.text(202, "explicit") })
    router.put("/item", async (request: Request) => { Response.bytes(201, "application/octet-stream", request.body()) })
    router.delete("/item", async (request: Request) => { Response.text(204, "") })
    router.route("OPTIONS", "/item", async (request: Request) => {
        Response.text(200, "options").withHeader("Access-Control-Allow-Origin", "*")
    })
    print(listener.port())
    await router.serve(listener, 2)
}
''', encoding="utf-8")
        for profile, app in self.profiles(package):
            with self.subTest(profile=profile), server(app) as (port, _, _):
                def request(method, path, body=None):
                    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                    try:
                        connection.request(method, path, body=body)
                        response = connection.getresponse()
                        return response.status, dict(response.getheaders()), response.read()
                    finally:
                        connection.close()

                status, fields, body = request("HEAD", "/fallback")
                self.assertEqual((status, fields["Content-Length"], body), (200, "4", b""))
                status, fields, body = request("HEAD", "/item")
                self.assertEqual((status, fields["Content-Length"], body), (202, "8", b""))
                status, fields, body = request("POST", "/item")
                self.assertEqual((status, fields["Allow"]), (405, "GET, HEAD, PUT, DELETE, OPTIONS"))
                status, fields, body = request("PUT", "/item", b"\xff\x00")
                self.assertEqual((status, fields["Content-Type"], body), (201, "application/octet-stream", b"\xff\x00"))
                status, fields, body = request("DELETE", "/item")
                self.assertEqual((status, "Content-Length" in fields, body), (204, False, b""))
                status, fields, body = request("OPTIONS", "/item")
                self.assertEqual((status, fields["Access-Control-Allow-Origin"], body), (200, "*", b"options"))
                self.assertEqual(request("DELETE", "/missing")[0], 404)

    def test_shared_listener_and_request_lifetimes(self):
        package = self.root / "lifetimes"
        (package / "src").mkdir(parents=True)
        (package / "lyra.json5").write_text("{name:'test/http-lifetimes',version:'1.0.0'}")
        (package / "src/main.ly").write_text(HTTP_HELPERS + '''
import std.http.{Busy}
import std.http.{Listener}
import std.http.{Request}

fn tryClose(listener: Listener): Bool {
    try { listener.close(); true } catch busy: Busy { false }
}

async fn raceListener(): Unit {
    val listener = requireHttp(listen(0))
    val accepting = parallel { listener.accept(20) }
    val closing = parallel { tryClose(listener) }
    val closedImmediately = await closing
    val accepted = await accepting
    #assert(failed(accepted, HttpErrorKind.Timeout) || failed(accepted, HttpErrorKind.Closed) ||
        failed(accepted, HttpErrorKind.Busy), "accept sees timeout or concurrent close, not an invalid descriptor")
    if !closedImmediately {
        #assert(failed(accepted, HttpErrorKind.Timeout), "busy close leaves accept and socket intact")
    }
    listener.close()
    #assert(failed(listener.accept(0), HttpErrorKind.Closed), "close releases the listener once")
}

fn sent(result: Result<Unit, HttpError>): Bool {
    match result {
        Ok(_) => true,
        Err(error) => {
            #assert(error.kind == HttpErrorKind.Busy || error.kind == HttpErrorKind.Closed, "competing response is rejected")
            false
        },
    }
}

async fn main(): Unit {
    for attempt in 0..<8 { await raceListener() }
    val listener = requireHttp(listen(0))
    defer listener.close()
    print(listener.port())
    requireHttp(await listener.withAccepted<Unit>(5000, async (request: Request) => {
        val first = parallel { request.respond(Response.text(200, "first")) }
        val second = parallel { request.respond(Response.text(200, "second")) }
        val firstSent = sent(await first)
        val secondSent = sent(await second)
        #assert(firstSent != secondSent, "exactly one response uses the socket")
    }))
}
''', encoding="utf-8")
        for profile, app in self.profiles(package):
            with self.subTest(profile=profile), server(app) as (port, process, _):
                status, media_type, body = exchange(port, "GET", "/")
                self.assertEqual((status, media_type), (200, "text/plain"))
                self.assertIn(body, (b"first", b"second"))
                self.assertEqual(process.wait(timeout=5), 0, process.stderr.read())

    def test_cancel_disconnect_wait_without_closing_connection(self):
        package = self.root / "wait-disconnect"
        (package / "src").mkdir(parents=True)
        (package / "lyra.json5").write_text("{name:'test/http-wait-disconnect',version:'1.0.0'}")
        (package / "src/main.ly").write_text(HTTP_HELPERS + '''
import std.http.{Request}
import std.io.{readLine}

async fn observe(work: Task<Unit>): Unit { await work }

async fn main(): Unit {
    val listener = requireHttp(listen(0))
    defer listener.close()
    print(listener.port())
    requireHttp(await listener.withAccepted<Unit>(5000, async (request: Request) => {
        val waiting = spawn blocking {
            print("waiting")
            #assert(!request.waitDisconnect(20000), "client closed instead of cancelling")
        }
        await parallel { #assert(match readLine() { Ok(Some(line)) => line == "cancel", _ => false }, "cancel gate") }
        waiting.cancel()
        taskGroup { spawn observe(waiting); }
        #assert(!request.disconnected() && request.watchDisconnect(), "connection remains usable")
        requireHttp(await blocking { request.respond(Response.text(200, "still connected")) })
        await parallel { #assert(match readLine() { Ok(Some(line)) => line == "closed", _ => false }, "close gate") }
        #assert(!Task.cancelled(), "local response close cancelled its own handler")
        print("finished")
    }))
}
''', encoding="utf-8")
        for profile, app in self.profiles(package):
            with self.subTest(profile=profile), server(app) as (port, process, output):
                with socket.create_connection(("127.0.0.1", port), timeout=5) as connection:
                    connection.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
                    self.assertEqual(output.line(), b"waiting\n")
                    process.stdin.write(b"cancel\n")
                    process.stdin.flush()
                    response = http.client.HTTPResponse(connection)
                    response.begin()
                    self.assertEqual((response.status, response.read()), (200, b"still connected"))
                    self.assertEqual(connection.recv(1), b"")
                    process.stdin.write(b"closed\n")
                    process.stdin.flush()
                    self.assertEqual(output.line(), b"finished\n")
                self.assertEqual(process.wait(timeout=5), 0, process.stderr.read())

    def test_query_snapshot_is_checked_without_running_a_prepare_step(self):
        package = self.root / 'service'
        shutil.copytree(SERVICE, package, ignore=shutil.ignore_patterns('build', '.lyra'))
        library = package / 'users'
        prepare = [sys.executable, library / 'tools/prepareQuery.py']
        subprocess.run([*prepare, '--check'], check=True, capture_output=True)
        schema = library / 'inputs/schema.sql'
        schema.write_bytes(schema.read_bytes().replace(b'\n', b'\r\n') + b'CREATE TABLE events(id INTEGER PRIMARY KEY);\r\n')
        def check():
            return subprocess.run([COMPILER, 'check', package, '--locked', '--offline', '--diagnostics=json'],
                                  capture_output=True, timeout=20)
        mismatch = check()
        self.assertNotEqual(mismatch.returncode, 0)
        diagnostic, = json.loads(mismatch.stderr)
        self.assertEqual(diagnostic['code'], 'LYM0002', diagnostic)
        source = library / 'src/users.ly'
        self.assertEqual(Path(diagnostic['file']).resolve(), source.resolve())
        self.assertEqual(diagnostic['range']['start']['offset'], source.read_bytes().index(b'"SELECT name'))
        subprocess.run(prepare, check=True, capture_output=True)
        refreshed = check()
        self.assertEqual(refreshed.returncode, 0, refreshed.stderr.decode())
        saved = (library / 'inputs/userById.snapshot').read_bytes()
        (library / 'tools/userById.sql').write_text('SELECT typo FROM users WHERE id=?1', encoding='utf-8')
        failed = subprocess.run(prepare, capture_output=True)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn(b'typo', failed.stderr)
        self.assertEqual((library / 'inputs/userById.snapshot').read_bytes(), saved)
        self.assertEqual(check().returncode, 0)

    def test_disconnect_and_framing(self):
        package = self.root / "listener"
        (package / "src").mkdir(parents=True)
        (package / "lyra.json5").write_text("{name:'test/http',version:'1.0.0'}")
        (package / "src/main.ly").write_text(HTTP_HELPERS + r'''
import std.io.{readLine}
fn main(): Unit {
    val server = requireHttp(listen(0))
    print(server.port())
    val first = requireHttp(server.accept(5000))
    print(first.method())
    readLine()
    #assert(first.waitDisconnect(5000) && first.disconnected(), "client still connected")
    first.close()
    val second = requireHttp(server.accept(5000))
    val headers = second.headers("X-Test")
    #assert(headers.size == 3 && headers[0] == "one" && headers[1] == "two" && headers[2] == "", "header order, case and empty value")
    #assert(failed(second.header("x-test"), HttpErrorKind.DuplicateHeader), "a repeated header is not single-valued")
    #assert(requireHttp(second.header("absent")) == None && requireHttp(second.header("HOST")) == Some("localhost"), "absent and unique header")
    #assert(match requireHttp(second.header("x-long")) { Some(value) => value.size == 8500, None => false }, "fragmented header")
    #assert(second.cookie("session") == Some("x=y") && second.cookie("blank") == Some(""), "cookies in repeated fields")
    #assert(second.cookie("Session") == None && second.cookie("unknown") == None, "cookie names are case-sensitive")
    #assert(failed(second.text(), HttpErrorKind.InvalidUtf8), "byte body does not silently become text")
    val response = Response.bytes(200, "application/octet-stream", second.body()).withHeader("X-Echo", "present")
    #assert(failed(second.respond(response.withHeader("content-length", "999")), HttpErrorKind.Invalid), "managed framing")
    #assert(failed(second.respond(response.withHeader("X-Bad", "value\r\nX-Fake: value")), HttpErrorKind.Invalid), "invalid header")
    requireHttp(second.respond(response))
    #assert(failed(second.respond(response), HttpErrorKind.Closed), "write after close")
    #assert(failed(server.accept(5000), HttpErrorKind.Malformed), "malformed input")
    #assert(failed(server.accept(5000), HttpErrorKind.InvalidUtf8), "invalid text header")
    #assert(failed(server.accept(5000), HttpErrorKind.TooLarge), "target size limit")
    #assert(failed(server.accept(5000), HttpErrorKind.TooLarge), "header size limit")
    for attempt in 0..<3 {
        #assert(failed(server.accept(5000), HttpErrorKind.Malformed), "require exactly one valid Host")
    }
    #assert(failed(server.accept(5000), HttpErrorKind.UnsupportedExpectation), "reject Expect before reading the body")
    server.close()
    #assert(failed(server.accept(5000), HttpErrorKind.Closed), "accept after close")
}
''')
        for profile, app in self.profiles(package):
            with self.subTest(profile=profile), server(app) as (port, process, output):
                with socket.create_connection(("127.0.0.1", port), 5) as drop:
                    drop.sendall(b"GET /hold HTTP/1.1\r\nHost: localhost\r\n\r\n")
                    self.assertEqual(output.line(), b"GET\n")
                    reset_on_close(drop)
                process.stdin.write(b"continue\n")
                process.stdin.flush()
                with socket.create_connection(("127.0.0.1", port), 5) as echo:
                    echo.sendall(b"POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n"
                                 b"X-TEST: one\t \r\nx-test: two\r\nX-Test:\r\n"
                                 b'Cookie: other=1; session="x=y"\r\ncOOkie: blank=\r\nX-Long: ' + b"z" * 8500 + b"\r\n\r\n")
                    echo.sendall(b"2\r\nA\x00\r\n2\r\n\xffC\r\n0\r\nX-Test: trailer\r\n\r\n")
                    answer = http.client.HTTPResponse(echo)
                    answer.begin()
                    self.assertEqual((answer.status, answer.getheader("X-Echo"), answer.read()), (200, "present", b"A\x00\xffC"))
                for raw, expected in (
                    (b"not a request\r\n\r\n", 400),
                    (b"GET / HTTP/1.1\r\nHost: x\r\nX-Not-Text: \xff\r\n\r\n", 400),
                    (b"GET /" + b"a" * 9000 + b" HTTP/1.1\r\nHost: x\r\n\r\n", 413),
                    (b"GET / HTTP/1.1\r\nHost: x\r\nX-Large: " + b"a" * 32768 + b"\r\n\r\n", 413),
                    (b"GET / HTTP/1.1\r\n\r\n", 400),
                    (b"GET / HTTP/1.1\r\nHost: x\r\nhost: x\r\n\r\n", 400),
                    (b"GET / HTTP/1.1\r\nHost: user@localhost\r\n\r\n", 400),
                    (b"POST / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n", 417),
                ):
                    with socket.create_connection(("127.0.0.1", port), 5) as sock:
                        sock.settimeout(2)
                        sock.sendall(raw)
                        response = http.client.HTTPResponse(sock)
                        response.begin()
                        self.assertEqual(response.status, expected)
                self.assertEqual(process.wait(timeout=5), 0, process.stderr.read())

    def test_disconnect_cancels_owner_before_sqlite_worker_releases_its_cursor(self):
        package = self.root / "async-listener"
        shutil.copytree(CANCELLATION, package, ignore=shutil.ignore_patterns("build", "lyra.lock"))
        for profile, app in self.profiles(package):
            with self.subTest(profile=profile):
                database = self.root / (profile + ".db")
                with contextlib.closing(sqlite3.connect(database, timeout=0)) as observer:
                    observer.executescript("CREATE TABLE rows(id INTEGER PRIMARY KEY); INSERT INTO rows VALUES(7),(8);")
                    with server(app, database) as (port, process, output):
                        with socket.create_connection(("127.0.0.1", port), timeout=5) as drop:
                            drop.sendall(b"GET /hold HTTP/1.1\r\nHost: localhost\r\n\r\n")
                            self.assertEqual(output.line(), b"cursor open\n")
                            reset_on_close(drop)
                        self.assertEqual(output.line(), b"cancellation observed\n")
                        self.assertIsNone(process.poll())
                        with self.assertRaisesRegex(sqlite3.OperationalError, "database is locked"):
                            observer.execute("BEGIN EXCLUSIVE")
                        process.stdin.write(b"continue\n")
                        process.stdin.flush()
                        self.assertEqual(exchange(port, "GET", "/next"), (200, "text/plain", b"ready"))
                        self.assertEqual(process.wait(timeout=5), 0)
                        self.assertEqual(output.remaining(), b"cursor closed\ndatabase closed\nrequest closed\nrequest group closed\n")
                        self.assertEqual(process.stderr.read(), b"")
                    observer.execute("BEGIN EXCLUSIVE")
                    observer.rollback()


if __name__ == "__main__":
    unittest.main()
