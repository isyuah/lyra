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
import std.http.{HttpServerOptions}
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
        line = output.line()
        if not line:
            raise RuntimeError(f"server exited ({process.wait(timeout=5)}): {process.stderr.read().decode(errors='replace')}")
        yield int(line), process, output


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

    def test_native_async_extern_is_private_to_the_sdk(self):
        package = self.root / "native-extern"
        (package / "src").mkdir(parents=True)
        (package / "lyra.json5").write_text("{name:'test/http-native-extern',version:'1.0.0'}")
        source = package / "src/main.ly"
        for declaration in ("extern fn hostTask(): Task<Int>;", "extern fn lyraHttpListenerCloseAsync(handle: CPtr): Task<Unit>;"):
            with self.subTest(declaration=declaration):
                source.write_text(declaration + "\nfn main(): Unit {}\n", encoding="utf-8")
                result = subprocess.run([COMPILER, "check", package, "--diagnostics=json"], capture_output=True, timeout=20)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("LYF0001", [diagnostic["code"] for diagnostic in json.loads(result.stderr)])

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
                    lock = sqlite3.connect(database, timeout=0)
                    lock.execute("BEGIN IMMEDIATE")
                    try:
                        busy = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                        busy.request("POST", "/users", body=json.dumps({"name": "queued"}), headers={"Content-Type": "application/json"})
                        response = busy.getresponse()
                        self.assertEqual((response.status, response.read(), response.getheader("Retry-After")), (503, b"database busy", "1"))
                        busy.close()
                    finally:
                        lock.rollback()
                        lock.close()
                    created_status, created_type, created_body = exchange(port, "POST", "/users", json.dumps({"name": "queued"}).encode())
                    self.assertEqual((created_status, created_type, json.loads(created_body)), (201, "application/json", {"id": 10, "name": "queued"}))
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
    val _ = await request.waitDisconnect(15000)
    Response.text(200, "released")
}
async fn ready(request: Request): Response { Response.text(200, "ready") }
async fn main(): Unit {
    val listener = requireHttp(listen(0, options: HttpServerOptions(capacity: 1)))
    val router = Router()
    router.get("/hold", (request: Request) => { hold(request) })
    router.get("/ready", (request: Request) => { ready(request) })
    print(listener.port())
    await router.serve(listener)
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

    def test_slow_connections_and_a_busy_worker_do_not_block_ready_requests(self):
        package = self.root / "backpressure"
        (package / "src").mkdir(parents=True)
        (package / "lyra.json5").write_text("{name:'test/http-backpressure',version:'1.0.0'}")
        (package / "src/main.ly").write_text(HTTP_HELPERS + '''
import std.http.{Request, Router}
import std.io.{readLine}

async fn main(): Unit {
    val listener = requireHttp(listen(0, options: HttpServerOptions(capacity: 2, idleReadTimeoutMs: Some(2000))))
    val router = Router()
    router.get("/hold", async (request: Request) => {
        await blocking {
            print("worker occupied")
            #assert(match readLine() { Ok(Some(line)) => line == "release", _ => false })
        }
        Response.text(200, "released")
    })
    router.get("/ready", async (request: Request) => { Response.text(200, "ready") })
    print(listener.port())
    await router.serve(listener)
}
''', encoding="utf-8")

        def connect(port, request):
            connection = socket.create_connection(("127.0.0.1", port), timeout=5)
            if request:
                connection.sendall(request)
            return connection

        def response(connection):
            with contextlib.closing(http.client.HTTPResponse(connection)) as reply:
                reply.begin()
                return reply.status, reply.getheader("Retry-After"), reply.read()

        partial = b"GET /ready HTTP/1.1\r\nHost:"
        for profile, app in self.profiles(package):
            with self.subTest(profile=profile, phase="ready"), server(app) as (port, process, output), contextlib.ExitStack() as clients:
                slow = clients.enter_context(connect(port, partial))
                held = clients.enter_context(connect(port, b"GET /hold HTTP/1.1\r\nHost: localhost\r\n\r\n"))
                self.assertEqual(output.line(), b"worker occupied\n")
                with connect(port, b"GET /ready HTTP/1.1\r\nHost: localhost\r\n\r\n") as healthy:
                    healthy.shutdown(socket.SHUT_WR)
                    self.assertEqual(response(healthy), (200, None, b"ready"))
                process.stdin.write(b"release\n")
                process.stdin.flush()
                self.assertEqual(response(held), (200, None, b"released"))
                self.assertEqual(response(slow), (408, None, b""))
            with self.subTest(profile=profile, phase="overload"), server(app) as (port, _, _), contextlib.ExitStack() as clients:
                for _ in range(4):
                    clients.enter_context(connect(port, partial))
                with connect(port, None) as overloaded:
                    self.assertEqual(response(overloaded), (503, "1", b""))

    def test_cold_operations_and_expiration_of_unclaimed_requests(self):
        package = self.root / "claim"
        (package / "src").mkdir(parents=True)
        (package / "lyra.json5").write_text("{name:'test/http-claim',version:'1.0.0'}")
        (package / "src/main.ly").write_text(HTTP_HELPERS + '''
import std.io.{readLine}

async fn main(): Unit {
    val listener = requireHttp(listen(0, options: HttpServerOptions(capacity: 1, claimTimeoutMs: 150)))
    val _ = listener.accept()
    print(listener.port())
    val stale = requireHttp(await listener.accept(5000))
    print(stale.target())
    await blocking { val _ = readLine() }
    #assert(failed(stale.claim(), HttpErrorKind.Closed), "an expired offer was claimed")
    val active = requireHttp(await listener.accept(5000))
    requireHttp(active.claim())
    val _ = active.respond(Response.text(500, "cold response ran"))
    await sleepMs(250)
    requireHttp(await active.respond(Response.text(200, "claimed")))
    await listener.closeAndWait()
}
''', encoding="utf-8")
        for profile, app in self.profiles(package):
            with self.subTest(profile=profile), server(app) as (port, process, output):
                with socket.create_connection(("127.0.0.1", port), timeout=5) as stale:
                    stale.sendall(b"GET /unclaimed HTTP/1.1\r\nHost: localhost\r\n\r\n")
                    self.assertEqual(output.line(), b"/unclaimed\n")
                    self.assertEqual(stale.recv(1), b"")
                process.stdin.write(b"continue\n")
                process.stdin.flush()
                self.assertEqual(exchange(port, "GET", "/claimed"), (200, "text/plain", b"claimed"))
                self.assertEqual(process.wait(timeout=5), 0, process.stderr.read())

    def test_keep_alive_pipeline_request_identity_and_idle_expiration(self):
        package = self.root / 'keep-alive'
        (package / 'src').mkdir(parents=True)
        (package / 'lyra.json5').write_text("{name:'test/keep-alive',version:'1.0.0'}")
        (package / 'src/main.ly').write_text(HTTP_HELPERS + '''
async fn main(): Unit {
    val listener = requireHttp(listen(0, options: HttpServerOptions(capacity: 1, maxBodyBytes: 1048576,
        maxRequestBytes: 2097152, keepAliveTimeoutMs: 200, maxRequestsPerConnection: 3)))
    print(listener.port())
    val first = requireHttp(await listener.accept(5000))
    #assert(first.target() == "/first" && first.body().size == 200000)
    requireHttp(await first.respond(Response.bytes(200, "application/octet-stream", first.body())))
    for index in 2..3 {
        val next = requireHttp(await listener.accept(5000))
        first.close()
        await first.closeAndWait()
        #assert(first.target() == "/first" && first.body().size == 200000)
        #assert(failed(await first.respond(Response.text(200, "stale")), HttpErrorKind.Closed))
        #assert(next.target() == "/${index}")
        requireHttp(await next.respond(Response.text(200, "${index}")))
    }
    for index in 0..<4 {
        val request = requireHttp(await listener.accept(5000))
        requireHttp(await request.respond(Response.text(200, request.target())))
    }
    #assert(failed(await listener.accept(5000), HttpErrorKind.TooLarge))
    await listener.closeAndWait()
}
''', encoding='utf-8')

        def read_response(stream):
            status = int(stream.readline().split()[1])
            headers = {}
            while line := stream.readline().rstrip(b'\r\n'):
                name, value = line.split(b':', 1)
                headers[name.lower()] = value.strip()
            body = stream.read(int(headers.get(b'content-length', b'0')))
            return status, headers, body

        for profile, app in self.profiles(package):
            with self.subTest(profile=profile), server(app) as (port, process, _):
                with socket.create_connection(('127.0.0.1', port), timeout=5) as connection, connection.makefile('rb') as stream:
                    body = bytes(range(250)) * 800
                    connection.sendall(b'POST /first HTTP/1.1\r\nHost: localhost\r\nContent-Length: 200000\r\n\r\n' + body)
                    status, headers, received = read_response(stream)
                    self.assertEqual((status, headers[b'connection'], received), (200, b'keep-alive', body))
                    connection.sendall(b'GET /2 HTTP/1.1\r\nHost: localhost\r\n\r\nGET /3 HTTP/1.1\r\nHost: localhost\r\n\r\n')
                    status, headers, received = read_response(stream)
                    self.assertEqual((status, headers[b'connection'], received), (200, b'keep-alive', b'2'))
                    status, headers, received = read_response(stream)
                    self.assertEqual((status, headers[b'connection'], received), (200, b'close', b'3'))
                    self.assertEqual(stream.read(1), b'')
                with socket.create_connection(('127.0.0.1', port), timeout=5) as connection, connection.makefile('rb') as stream:
                    connection.sendall(b'GET /old HTTP/1.0\r\n\r\n')
                    status, headers, received = read_response(stream)
                    self.assertEqual((status, headers[b'connection'], received), (200, b'close', b'/old'))
                    self.assertEqual(stream.read(1), b'')
                with socket.create_connection(('127.0.0.1', port), timeout=5) as connection, connection.makefile('rb') as stream:
                    connection.sendall(b'GET /old-persistent HTTP/1.0\r\nConnection: keep-alive\r\n\r\n')
                    status, headers, received = read_response(stream)
                    self.assertEqual((status, headers[b'connection'], received), (200, b'keep-alive', b'/old-persistent'))
                    connection.sendall(b'GET /close HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
                    status, headers, received = read_response(stream)
                    self.assertEqual((status, headers[b'connection'], received), (200, b'close', b'/close'))
                    self.assertEqual(stream.read(1), b'')
                with socket.create_connection(('127.0.0.1', port), timeout=5) as connection, connection.makefile('rb') as stream:
                    connection.sendall(b'GET /idle HTTP/1.1\r\nHost: localhost\r\n\r\n')
                    self.assertEqual(read_response(stream)[2], b'/idle')
                    self.assertEqual(stream.read(1), b'')
                with socket.create_connection(('127.0.0.1', port), timeout=5) as connection, connection.makefile('rb') as stream:
                    connection.sendall(b'POST /large HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1048577\r\n\r\n')
                    self.assertEqual(read_response(stream)[0], 413)
                self.assertEqual(process.wait(timeout=5), 0, process.stderr.read())

    def test_handler_bounds_failure_does_not_stop_the_server(self):
        package = self.root / "handler-error"
        (package / "src").mkdir(parents=True)
        (package / "lyra.json5").write_text("{name:'test/handler-error',version:'1.0.0'}")
        (package / "src/main.ly").write_text(HTTP_HELPERS + '''
import std.http.{Request}
import std.http.{Router}
async fn main(): Unit {
    val listener = requireHttp(listen(0, options: HttpServerOptions(capacity: 1)))
    val router = Router()
    router.get("/fail", async (request: Request) => {
        defer print("handler cleanup")
        Response.text(200, ["one"][1])
    })
    router.get("/ready", async (request: Request) => { Response.text(200, "ready") })
    print(listener.port())
    await router.serve(listener)
}
''', encoding="utf-8")
        for profile, app in self.profiles(package):
            with self.subTest(profile=profile), server(app) as (port, _, output):
                self.assertEqual(exchange(port, "GET", "/fail")[0], 500)
                self.assertEqual(output.line().strip(), b"handler cleanup")
                self.assertEqual(exchange(port, "GET", "/ready"), (200, "text/plain", b"ready"))

    def test_router_shutdown_drains_active_requests_before_stopping(self):
        package = self.root / "shutdown"
        (package / "src").mkdir(parents=True)
        (package / "lyra.json5").write_text("{name:'test/http-shutdown',version:'1.0.0'}")
        (package / "src/main.ly").write_text(HTTP_HELPERS + '''
import std.http.{Request}
import std.http.{Router}
import std.http.{Shutdown}
import std.io.{readLine}

async fn main(): Unit {
    val listener = requireHttp(listen(0, options: HttpServerOptions(capacity: 1, graceMs: 1000)))
    val router = Router()
    router.get("/delayed", async (request: Request) => {
        print("handler-started")
        defer print("handler-cleanup")
        await sleepMs(80)
        Response.text(200, "done")
    })
    val shutdown = Shutdown()
    val service = spawn router.serve(listener, Some(shutdown))
    print(listener.port())
    await parallel { #assert(match readLine() { Ok(Some(line)) => line == "stop", _ => false }) }
    shutdown.request()
    await service
    print("server-stopped")
}
''', encoding="utf-8")
        for profile, app in self.profiles(package):
            with self.subTest(profile=profile), server(app) as (port, process, output):
                connection = socket.create_connection(("127.0.0.1", port), timeout=5)
                self.addCleanup(connection.close)
                connection.sendall(b"GET /delayed HTTP/1.1\r\nHost: localhost\r\n\r\n")
                try:
                    started = output.line()
                except TimeoutError:
                    status = process.poll()
                    if status is None:
                        process.terminate()
                        status = process.wait(timeout=5)
                    self.fail(f"handler did not start (exit {status}): {process.stderr.read().decode(errors='replace')}")
                self.assertEqual(started, b"handler-started\n")
                process.stdin.write(b"stop\n")
                process.stdin.flush()
                response = http.client.HTTPResponse(connection)
                response.begin()
                self.assertEqual((response.status, response.read()), (200, b"done"))
                self.assertEqual(process.wait(timeout=5), 0, process.stderr.read())
                self.assertEqual(output.remaining(), b"handler-cleanup\nserver-stopped\n")

    def test_router_shutdown_waits_for_scoped_sqlite_write(self):
        package = self.root / "shutdown-sqlite"
        (package / "src").mkdir(parents=True)
        (package / "lyra.json5").write_text("{name:'test/http-shutdown-sqlite',version:'1.0.0'}")
        (package / "src/main.ly").write_text(HTTP_HELPERS + '''
import std.http.{Request}
import std.http.{Router}
import std.http.{Shutdown}
import std.io.{readLine}
import std.os.{args}
import std.sqlite.{DbError}
import std.sqlite.{withConnection}

extern fn uv_sleep(milliseconds: Int32): Unit;

async fn writeDatabase(path: String): Result<Unit, DbError> {
    await blocking {
        withConnection<Unit>(path) { db =>
            db.transaction<Unit>() { tx =>
                tx.exec("CREATE TABLE IF NOT EXISTS writes(value INTEGER)")?
                tx.exec("INSERT INTO writes VALUES(1)")?
                uv_sleep(250)
                Ok({})
            }
        }
    }
}

async fn main(): Unit {
    val arguments = match args() { Ok(value) => value, Err(error) => throw Exception(error.message()) }
    val listener = requireHttp(listen(0, options: HttpServerOptions(capacity: 1, graceMs: 2000)))
    val router = Router()
    router.get("/write", async (request: Request) => {
        print("write-started")
        defer print("handler-cleanup")
        match await writeDatabase(arguments[0]) {
            Ok(_) => Response.text(200, "committed"),
            Err(error) => Response.text(500, error.message()),
        }
    })
    val shutdown = Shutdown()
    val service = spawn router.serve(listener, Some(shutdown))
    print(listener.port())
    await parallel { #assert(match readLine() { Ok(Some(line)) => line == "stop", _ => false }) }
    shutdown.request()
    await service
    print("server-stopped")
}
''', encoding="utf-8")
        for profile, app in self.profiles(package):
            database_path = self.root / (profile + ".sqlite")
            with self.subTest(profile=profile), server(app, database_path) as (port, process, output):
                connection = socket.create_connection(("127.0.0.1", port), timeout=5)
                try:
                    connection.sendall(b"GET /write HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
                    self.assertEqual(output.line(), b"write-started\n")
                    process.stdin.write(b"stop\n")
                    process.stdin.flush()
                    response = http.client.HTTPResponse(connection)
                    response.begin()
                    self.assertEqual((response.status, response.read()), (200, b"committed"))
                finally:
                    connection.close()
                self.assertEqual(output.line(), b"handler-cleanup\n")
                self.assertEqual(process.wait(timeout=5), 0, process.stderr.read())
                self.assertEqual(output.remaining(), b"server-stopped\n")
                with contextlib.closing(sqlite3.connect(database_path)) as database:
                    self.assertEqual(database.execute("SELECT value FROM writes").fetchall(), [(1,)])

    def test_shutdown_cancels_at_grace_deadline_but_joins_native_write(self):
        package = self.root / "shutdown-deadline"
        (package / "src").mkdir(parents=True)
        (package / "lyra.json5").write_text("{name:'test/http-shutdown-deadline',version:'1.0.0'}")
        (package / "src/main.ly").write_text(HTTP_HELPERS + '''
import std.http.{Request, Router, Shutdown}
import std.io.{readLine}
import std.os.{args}
import std.sqlite.{DbError, withConnection}

async fn write(path: String): Result<Unit, DbError> {
    await blocking {
        withConnection<Unit>(path) { db =>
            db.transaction<Unit>() { tx =>
                tx.exec("INSERT INTO writes VALUES (1)")?
                print("transaction open")
                #assert(match readLine() { Ok(Some(line)) => line == "commit", _ => false })
                Ok({})
            }
        }
    }
}

async fn observe(task: Task<Result<Unit, DbError>>): Unit {
    defer print("observer stopped")
    print("observer started")
    val _ = await task
}

async fn main(): Unit {
    val arguments = match args() { Ok(value) => value, Err(error) => throw Exception(error.message()) }
    val listener = requireHttp(listen(0, options: HttpServerOptions(capacity: 2, graceMs: 0)))
    val shutdown = Shutdown()
    val router = Router()
    router.get("/hold", async (request: Request) => {
        defer print("handler released")
        taskGroup {
            val writing = spawn write(arguments[0])
            spawn observe(writing);
            val _ = await writing
            print("handler resumed")
        };
        Response.text(200, "unexpected response")
    })
    router.get("/stop", async (request: Request) => {
        shutdown.request()
        Response.text(200, "stopping")
    })
    print(listener.port())
    await router.serve(listener, Some(shutdown))
    print("stopped")
}
''', encoding="utf-8")
        for profile, app in self.profiles(package):
            database = self.root / f"{profile}.sqlite"
            with contextlib.closing(sqlite3.connect(database, timeout=0)) as connection:
                connection.execute("CREATE TABLE writes(value INTEGER)")
                with self.subTest(profile=profile), server(app, database) as (port, process, output):
                    with socket.create_connection(("127.0.0.1", port), timeout=5) as held:
                        held.sendall(b"GET /hold HTTP/1.1\r\nHost: localhost\r\n\r\n")
                        self.assertEqual({output.line(), output.line()}, {b"transaction open\n", b"observer started\n"})
                        with socket.create_connection(("127.0.0.1", port), timeout=5) as stop:
                            stop.sendall(b"GET /stop HTTP/1.1\r\nHost: localhost\r\n\r\n")
                            self.assertEqual(output.line(), b"observer stopped\n")
                            self.assertIsNone(process.poll())
                            with self.assertRaisesRegex(sqlite3.OperationalError, "database is locked"):
                                connection.execute("BEGIN IMMEDIATE")
                            process.stdin.write(b"commit\n")
                            process.stdin.flush()
                            self.assertEqual(process.wait(timeout=5), 0, process.stderr.read())
                            self.assertEqual(output.remaining(), b"handler released\nstopped\n")
                    self.assertEqual(connection.execute("SELECT value FROM writes").fetchall(), [(1,)])

    def test_mount_annotated_routes_across_modules_and_commit_atomically(self):
        package = self.root / "mounted"
        (package / "src/handlers").mkdir(parents=True)
        (package / "other/src").mkdir(parents=True)
        (package / "lyra.json5").write_text('''{name:'test/mounted',version:'1.0.0',
            dependencies:{other:{name:'test/other',version:'^1.0.0',path:'other'}}}''')
        (package / "other/lyra.json5").write_text("{name:'test/other',version:'1.0.0',kind:'lib'}")
        (package / "other/src/routes.ly").write_text('''
import std.http.{Router, Response}
#get("/other")
async fn privateRoute(): Response { Response.text(200, "other") }
pub fn install(router: Router): Unit { #mountRoutes(router) }
''')
        (package / "src/handlers/routes.ly").write_text('''
import std.http.{Response}
type Prefix = String
#get("/teams/{name}")
async fn team([path] name: String, [inject] prefix: Prefix): Response { Response.text(200, prefix + name) }
#get("/{section}/current")
async fn generic([path] section: String): Response { Response.text(200, section) }
#post("/teams/{name}")
async fn post([path] name: String): Response { Response.text(201, name) }
''')
        (package / "src/main.ly").write_text(HTTP_HELPERS + '''
import std.http.{Router, Request}
import std.sync.{AtomicInt}
import std.text.{StringOps}
import other

struct AppDeps(val prefix: String)
fn deps(calls: AtomicInt): AppDeps { val _ = calls.fetchAdd(1); AppDeps("team:") }
fn pick(router: Router, calls: AtomicInt): Router { val _ = calls.fetchAdd(1); router }

#get("/atomic")
async fn atomic(): Response { Response.text(200, "atomic") }

async fn main(): Unit {
    val router = Router()
    val calls = AtomicInt()
    #mountRoutes(pick(router, calls), deps(calls))
    #assert(calls.load() == 2)
    router.get("/teams/current", async (request: Request) => { Response.text(200, "literal") })
    val conflict = Router()
    conflict.get("/teams/{other}", async (request: Request) => { Response.text(200, "manual") })
    val rejected = try { #mountRoutes(conflict, AppDeps("unused")); false } catch error: Exception {
        #assert(error.message.startsWith("conflicting HTTP routes: GET /teams/{other} (manual registration) and GET /teams/{name}"))
        true
    }
    #assert(rejected)
    conflict.get("/atomic", async (request: Request) => { Response.text(200, "still available") })
    other.install(conflict)
    val listener = requireHttp(listen(0))
    print(listener.port())
    await router.serve(listener)
}
''', encoding="utf-8")
        for profile, app in self.profiles(package):
            with self.subTest(profile=profile), server(app) as (port, _, _):
                for method, target, status, body in [
                    ("GET", "/teams/alpha", 200, b"team:alpha"),
                    ("GET", "/teams/current", 200, b"literal"),
                    ("GET", "/otherSection/current", 200, b"otherSection"),
                    ("POST", "/teams/current", 201, b"current"),
                    ("GET", "/other", 404, b"not found"),
                ]:
                    self.assertEqual(exchange(port, method, target)[::2], (status, body))

    def test_annotated_route_rejects_duplicate_shapes_and_inexact_dependencies(self):
        package = self.root / "route-errors"
        (package / "src").mkdir(parents=True)
        (package / "lyra.json5").write_text("{name:'test/route-errors',version:'1.0.0'}")
        source = package / "src/main.ly"
        first = package / "src/first.ly"
        first.write_text('''import std.http.{Response}
#get("/caf%C3%A9/{one}")
async fn first(): Response { Response.text(200, "first") }
''', encoding="utf-8")
        source.write_text('''import std.http.{Response}
#get("/caf\u00e9/{other}")
async fn second(): Response { Response.text(200, "second") }
fn main(): Unit {}
''', encoding="utf-8")

        def diagnostics():
            result = subprocess.run([COMPILER, 'check', package, '--diagnostics=json'], capture_output=True, timeout=20)
            self.assertNotEqual(result.returncode, 0)
            return json.loads(result.stderr)

        duplicate, = diagnostics()
        self.assertEqual(duplicate["code"], "LYM0002")
        self.assertEqual(Path(duplicate["file"]), source)
        self.assertEqual(Path(duplicate["notes"][0]["file"]), first)
        source.write_text('''import std.http.{Response, Router}
struct AppDeps(val count: Int8)
#get("/x")
async fn count([inject] count: Int): Response { Response.text(200, count.toString()) }
fn main(): Unit { #mountRoutes(Router(), AppDeps(1)) }
''', encoding="utf-8")
        mismatch, = diagnostics()
        self.assertEqual((mismatch["code"], mismatch["expected"], mismatch["actual"]), ("LYT0001", "Int", "Int8"))
        self.assertEqual(Path(mismatch["file"]), source)

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
    await router.serve(listener)
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
import std.http.{Request}

async fn raceListener(): Unit {
    val listener = requireHttp(listen(0))
    val accepting = spawn listener.accept(20)
    val closing = parallel { listener.close() }
    await closing
    val accepted = await accepting
    #assert(failed(accepted, HttpErrorKind.Timeout) || failed(accepted, HttpErrorKind.Closed), "accept sees timeout or concurrent close")
    listener.close()
    await listener.closeAndWait()
    val closed = await listener.accept(0)
    #assert(failed(closed, HttpErrorKind.Closed), "close releases the listener once")
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
    print(listener.port())
    requireHttp(await listener.withAccepted<Unit>(5000, async (request: Request) => {
        val first = spawn request.respond(Response.text(200, "first"))
        val second = spawn request.respond(Response.text(200, "second"))
        val firstSent = sent(await first)
        val secondSent = sent(await second)
        #assert(firstSent != secondSent, "exactly one response uses the socket")
    }))
    await listener.closeAndWait()
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

async fn waitForPeer(request: Request): Bool {
    print("waiting")
    await request.waitDisconnect(20000)
}

async fn main(): Unit {
    val listener = requireHttp(listen(0))
    defer listener.close()
    print(listener.port())
    requireHttp(await listener.withAccepted<Unit>(5000, async (request: Request) => {
        val waiting = spawn waitForPeer(request)
        await parallel { #assert(match readLine() { Ok(Some(line)) => line == "cancel", _ => false }, "cancel gate") }
        waiting.cancel()
        val cancelled = awaitOutcome(waiting)
        #assert(match cancelled { Cancelled(_) => true, _ => false })
        val localClose = spawn request.waitDisconnect()
        requireHttp(await request.respond(Response.text(200, "still connected")))
        val lost = await localClose
        #assert(!lost && !request.disconnected(), "local response close looked like a peer disconnect")
        await parallel { #assert(match readLine() { Ok(Some(line)) => line == "closed", _ => false }, "close gate") }
        #assert(!Task.cancelled(), "local response close cancelled its own handler")
        print("finished")
    }))
}
''', encoding="utf-8")
        for profile, app in self.profiles(package):
            with self.subTest(profile=profile), server(app) as (port, process, output):
                with socket.create_connection(("127.0.0.1", port), timeout=5) as connection:
                    connection.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
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
        diagnostic = next(item for item in json.loads(mismatch.stderr) if item['code'] == 'LYM0002')
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
import std.http.{Listener}
import std.io.{readLine}

async fn expectRejection(server: Listener, kind: HttpErrorKind): Unit {
    val pending = spawn server.accept(5000)
    val probe = await server.accept(0)
    #assert(failed(probe, HttpErrorKind.Busy), "manual receiver was not registered")
    print("send invalid request")
    val rejected = await pending
    #assert(failed(rejected, kind), "wrong rejection kind")
}

async fn main(): Unit {
    val server = requireHttp(listen(0, options: HttpServerOptions(capacity: 1)))
    print(server.port())
    val first = requireHttp(await server.accept(5000))
    print(first.method())
    await blocking { val _ = readLine() }
    val lost = await first.waitDisconnect(5000)
    #assert(lost && first.disconnected(), "client still connected")
    await first.closeAndWait()
    val second = requireHttp(await server.accept(5000))
    val headers = second.headers("X-Test")
    #assert(headers.size == 3 && headers[0] == "one" && headers[1] == "two" && headers[2] == "", "header order, case and empty value")
    #assert(failed(second.header("x-test"), HttpErrorKind.DuplicateHeader), "a repeated header is not single-valued")
    #assert(requireHttp(second.header("absent")) == None && requireHttp(second.header("HOST")) == Some("localhost"), "absent and unique header")
    #assert(match requireHttp(second.header("x-long")) { Some(value) => value.size == 8500, None => false }, "fragmented header")
    #assert(second.cookie("session") == Some("x=y") && second.cookie("blank") == Some(""), "cookies in repeated fields")
    #assert(second.cookie("Session") == None && second.cookie("unknown") == None, "cookie names are case-sensitive")
    #assert(failed(second.text(), HttpErrorKind.InvalidUtf8), "byte body does not silently become text")
    val response = Response.bytes(200, "application/octet-stream", second.body()).withHeader("X-Echo", "present")
    val badLength = await second.respond(response.withHeader("content-length", "999"))
    val badHeader = await second.respond(response.withHeader("X-Bad", "value\r\nX-Fake: value"))
    #assert(failed(badLength, HttpErrorKind.Invalid), "managed framing")
    #assert(failed(badHeader, HttpErrorKind.Invalid), "invalid header")
    requireHttp(await second.respond(response))
    val after = await second.respond(response)
    #assert(failed(after, HttpErrorKind.Closed), "write after close")
    await expectRejection(server, HttpErrorKind.Malformed)
    await expectRejection(server, HttpErrorKind.InvalidUtf8)
    await expectRejection(server, HttpErrorKind.TooLarge)
    await expectRejection(server, HttpErrorKind.TooLarge)
    for attempt in 0..<3 {
        await expectRejection(server, HttpErrorKind.Malformed)
    }
    await expectRejection(server, HttpErrorKind.UnsupportedExpectation)
    await server.closeAndWait()
    val closed = await server.accept(5000)
    #assert(failed(closed, HttpErrorKind.Closed), "accept after close")
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
                    self.assertEqual(output.line(), b"send invalid request\n")
                    with socket.create_connection(("127.0.0.1", port), 5) as sock:
                        sock.settimeout(2)
                        sock.sendall(raw)
                        with contextlib.closing(http.client.HTTPResponse(sock)) as response:
                            response.begin()
                            self.assertEqual((response.status, response.read()), (expected, b""))
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
