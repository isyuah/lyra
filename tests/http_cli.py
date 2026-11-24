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
                assert_linkage(self, app, 'libsqlite3.so.0')

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
import store.findUser
import store.newUserCodec
import store.userCodec
import std.io.arg
import std.io.assert
import std.sqlite.DbErrorKind

fn main(): Unit {
    val database = arg(0)
    val missingTable = arg(1)
    assert(database.ok() && missingTable.ok(), "expected database paths")
    assert(match findUser(database.value, 7) {
        Ok(Some(user)) => {
            val codec = userCodec()
            match codec.decode(codec.encode(user)) { Ok(decoded) => decoded.id == 7 && decoded.name == "Ada", Err(_) => false }
        },
        _ => false,
    }, "expected user and JSON round trip")
    assert(match findUser(database.value, 8) { Ok(None) => true, _ => false }, "no row is not an error")
    assert(match findUser(missingTable.value, 7) {
        Err(error) => match error.kind { DbErrorKind.Sqlite => error.operation == "prepare", _ => false },
        Ok(_) => false,
    }, "expected typed database error")
    assert(match newUserCodec().decode(r#"{"name":"Ada","extra":true}"#) {
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
        (package / "src/main.ly").write_text('''
import std.http.listen
import std.http.Request
import std.http.Response
import std.http.Router
import std.io.assert
async fn hold(request: Request): Response {
    print("entered")
    await blocking { request.waitDisconnect(15000); }
    Response.text(200, "released")
}
async fn ready(request: Request): Response { Response.text(200, "ready") }
async fn main(): Unit {
    val listener = listen(0)
    assert(listener.status() == 0, "listen")
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
        (package / "src/main.ly").write_text('''
import std.http.listen
import std.io.readLine
import std.io.assert
fn main(): Unit {
    val server = listen(0)
    print(server.port())
    val first = server.accept(5000)
    print(first.method())
    readLine()
    assert(first.waitDisconnect(5000) && first.disconnected(), "client still connected")
    first.close()
    val second = server.accept(5000)
    assert(second.status() == 0, "second request")
    assert(second.respond(200, "text/plain", second.body()) == 0, "echo")
    assert(server.accept(5000).status() != 0, "malformed input")
    assert(server.accept(5000).status() != 0, "size limit")
    server.close()
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
                    echo.sendall(b"POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n")
                    echo.sendall(b"2\r\nA\x00\r\n2\r\nBC\r\n0\r\n\r\n")
                    answer = http.client.HTTPResponse(echo)
                    answer.begin()
                    self.assertEqual((answer.status, answer.read()), (200, b"A\x00BC"))
                for raw, expected in ((b"not a request\r\n\r\n", 400), (b"GET /" + b"a" * 9000 + b" HTTP/1.1\r\nHost: x\r\n\r\n", 413)):
                    with socket.create_connection(("127.0.0.1", port), 5) as sock:
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
