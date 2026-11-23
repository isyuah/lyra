import contextlib
import http.client
import json
import os
from pathlib import Path
import select
import shutil
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()
SERVICE = Path(__file__).resolve().parents[1] / "examples/http-sqlite"
CANCELLATION = Path(__file__).resolve().parent / "fixtures/http-cancel"


@contextlib.contextmanager
def server(executable, *arguments):
    process = subprocess.Popen(["stdbuf", "-oL", executable, *arguments], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               env={**os.environ, "UV_THREADPOOL_SIZE": "1"})
    try:
        if not select.select([process.stdout], [], [], 8)[0]:
            raise TimeoutError("listener did not report its bound port")
        yield int(process.stdout.readline()), process
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()


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
        self.root = Path(temporary.name)

    def profiles(self, package):
        for profile, flags in (("debug", []), ("release", ["--release"])):
            output = self.root / profile
            built = subprocess.run([COMPILER, "build", package, "-o", output, *flags], capture_output=True, timeout=45)
            self.assertEqual(built.returncode, 0, built.stderr.decode())
            yield profile, output

    def test_sqlite_service(self):
        package = self.root / "service"
        shutil.copytree(SERVICE, package, ignore=shutil.ignore_patterns("build"))
        subprocess.run([COMPILER, 'check', package, '--locked', '--offline'], check=True, capture_output=True, timeout=30)
        for profile, app in self.profiles(package):
            with self.subTest(profile=profile):
                database = self.root / (profile + ".db")
                name = '李"\\\n\0Ada'
                with sqlite3.connect(database) as connection:
                    connection.execute("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT)")
                    connection.execute("INSERT INTO users VALUES(7,?)", (name,))
                    connection.execute("INSERT INTO users VALUES(9,?)", ("x" * (2 * 1024 * 1024),))
                with server(app, database) as (port, process):
                    with socket.socket() as drop:
                        drop.settimeout(5)
                        drop.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                        drop.connect(("127.0.0.1", port))
                        drop.sendall(b"GET /users/9 HTTP/1.1\r\nHost: localhost\r\n\r\n")
                        self.assertEqual(drop.recv(1), b"H")
                        drop.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                    status, mime, body = exchange(port, "GET", "/users/7?view=one")
                    self.assertEqual((status, mime, json.loads(body)), (200, "application/json", {"id": 7, "name": name}))
                    self.assertEqual(exchange(port, "GET", "/users/8")[0], 404)
                    self.assertEqual(exchange(port, "GET", "/users/invalid")[0], 400)
                    self.assertEqual(exchange(port, "POST", "/users/7", b"body")[0], 405)
                    database.unlink()
                    self.assertEqual(exchange(port, "GET", "/users/7")[0], 500)
                    self.assertIsNone(process.poll())
                dynamic = subprocess.check_output(["readelf", "-dW", app], text=True)
                needed = {line.split("[")[1].split("]")[0] for line in dynamic.splitlines() if "(NEEDED)" in line}
                self.assertEqual(needed, {"libc.so.6", "libsqlite3.so.0"})

    def test_query_snapshot_is_checked_without_running_a_prepare_step(self):
        package = self.root / 'service'
        shutil.copytree(SERVICE, package, ignore=shutil.ignore_patterns('build', 'lyra.lock'))
        prepare = [sys.executable, package / 'tools/prepareQuery.py']
        subprocess.run([*prepare, '--check'], check=True, capture_output=True)
        schema = package / 'inputs/schema.sql'
        schema.write_text(schema.read_text() + 'CREATE TABLE events(id INTEGER PRIMARY KEY);\n')
        def check():
            return subprocess.run([COMPILER, 'check', package, '--diagnostics=json'], capture_output=True, timeout=20)
        mismatch = check()
        self.assertNotEqual(mismatch.returncode, 0)
        self.assertEqual(json.loads(mismatch.stderr)[0]['code'], 'LYM0002')
        subprocess.run(prepare, check=True, capture_output=True)
        self.assertEqual(check().returncode, 0)
        saved = (package / 'inputs/userById.snapshot').read_bytes()
        (package / 'tools/userById.sql').write_text('SELECT typo FROM users WHERE id=?1')
        failed = subprocess.run(prepare, capture_output=True)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn(b'typo', failed.stderr)
        self.assertEqual((package / 'inputs/userById.snapshot').read_bytes(), saved)
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
            with self.subTest(profile=profile), server(app) as (port, process):
                with socket.create_connection(("127.0.0.1", port), 5) as drop:
                    drop.sendall(b"GET /hold HTTP/1.1\r\nHost: localhost\r\n\r\n")
                    self.assertTrue(select.select([process.stdout], [], [], 5)[0])
                    self.assertEqual(process.stdout.readline(), b"GET\n")
                    drop.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
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
                    with server(app, database) as (port, process):
                        with socket.create_connection(("127.0.0.1", port), timeout=5) as drop:
                            drop.sendall(b"GET /hold HTTP/1.1\r\nHost: localhost\r\n\r\n")
                            self.assertTrue(select.select([process.stdout], [], [], 5)[0])
                            self.assertEqual(process.stdout.readline(), b"cursor open\n")
                            drop.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                        self.assertTrue(select.select([process.stdout], [], [], 5)[0])
                        self.assertEqual(process.stdout.readline(), b"cancellation observed\n")
                        self.assertIsNone(process.poll())
                        with self.assertRaisesRegex(sqlite3.OperationalError, "database is locked"):
                            observer.execute("BEGIN EXCLUSIVE")
                        process.stdin.write(b"continue\n")
                        process.stdin.flush()
                        self.assertEqual(exchange(port, "GET", "/next"), (200, "text/plain", b"ready"))
                        self.assertEqual(process.wait(timeout=5), 0)
                        self.assertEqual(process.stdout.read(), b"cursor closed\ndatabase closed\nrequest closed\nrequest group closed\n")
                        self.assertEqual(process.stderr.read(), b"")
                    observer.execute("BEGIN EXCLUSIVE")
                    observer.rollback()


if __name__ == "__main__":
    unittest.main()
