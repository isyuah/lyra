import contextlib
import http.server
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest

from platform_cli import executable, running


COMPILER = Path(sys.argv.pop(1)).resolve()
OPENSSL = os.environ.get('LYRA_TEST_OPENSSL') or shutil.which('openssl')


class FixtureHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *arguments):
        pass

    def handle(self):
        try:
            super().handle()
        except (ConnectionError, ssl.SSLEOFError, ssl.SSLZeroReturnError):
            self.close_connection = True

    def reply(self, status, content=b'', headers=()):
        try:
            self.send_response_only(status)
            self.send_header('Content-Length', str(len(content)))
            for name, value in headers:
                self.send_header(name, value)
            self.end_headers()
            if self.command != 'HEAD':
                self.wfile.write(content)
        except (ConnectionError, ssl.SSLEOFError, ssl.SSLZeroReturnError):
            self.close_connection = True

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        if self.path == '/data':
            self.reply(200, b'xy\x00\xff', [('Set-Cookie', 'one=1'), ('set-cookie', 'two=2'), ('X-Raw', '\xff')])
        elif self.path == '/redirect':
            self.reply(302, headers=[('Location', '/data')])
        elif self.path == '/redirect-elsewhere':
            self.reply(302, headers=[('Location', 'http://127.0.0.1:1/secret')])
        elif self.path == '/redirect-ambiguous':
            self.reply(302, headers=[('Location', '/data'), ('lOcAtIoN', '/elsewhere')])
        elif self.path == '/redirect-loop':
            self.reply(302, headers=[('Location', '/redirect-loop')])
        elif self.path == '/budget-start':
            self.reply(302, b'a' * 64, [('Location', '/budget-end')])
        elif self.path == '/budget-end':
            self.reply(200, b'b' * 96)
        elif self.path == '/large':
            self.reply(200, b'z' * 32768)
        elif self.path == '/large-header':
            self.reply(200, headers=[('X-Large', 'x' * 256)])
        elif self.path == '/slow':
            self.server.pending = self.connection
            self.server.arrived.set()
            self.server.release.wait(timeout=30)
            self.reply(200, b'too late')
        elif self.path == '/ok':
            self.reply(200, b'ok')
        elif self.path == 'http://proxy-target.invalid/through':
            self.reply(200, b'explicit proxy')
        else:
            self.reply(404, b'not found')

    def do_PUT(self):
        content = self.rfile.read(int(self.headers.get('Content-Length', '0')))
        self.reply(201, content, [('X-Echo', self.headers['X-Echo'])])


@contextlib.contextmanager
def server(tls=None):
    with http.server.ThreadingHTTPServer(('127.0.0.1', 0), FixtureHandler) as fixture:
        if tls:
            fixture.socket = tls.wrap_socket(fixture.socket, server_side=True)
        fixture.arrived = threading.Event()
        fixture.release = threading.Event()
        thread = threading.Thread(target=fixture.serve_forever, kwargs={'poll_interval': 0.05}, daemon=True)
        thread.start()
        try:
            yield fixture
        finally:
            fixture.release.set()
            fixture.shutdown()
            thread.join()


class HttpClient(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / 'src').mkdir()
        (self.root / 'lyra.json5').write_text('{name:"test/http-client",version:"0.1.0"}', encoding='utf-8')

    def programs(self, source):
        (self.root / 'src/main.ly').write_text(source, encoding='utf-8')
        for mode, flags in (('debug', []), ('release', ['--release'])):
            app = executable(self.root / mode)
            build = subprocess.run([COMPILER, 'build', self.root, *flags, '-o', app], capture_output=True, timeout=45)
            with self.subTest(mode=mode):
                self.assertEqual(build.returncode, 0, build.stderr.decode())
            if build.returncode == 0:
                yield app

    def test_binary_headers_requests_redirects_proxy_and_limits(self):
        for app in self.programs(r'''
import std.httpClient.{ClientResponse}
import std.httpClient.{HttpClientError}
import std.httpClient.{HttpClientOptions}
import std.httpClient.{HttpHeader}
import std.httpClient.{httpGet}
import std.httpClient.{httpRequest}
import std.os.{args}

fn require(result: Result<ClientResponse, HttpClientError>): ClientResponse {
    match result { Ok(response) => response, Err(problem) => throw Exception(problem.message()) }
}

fn show(result: Result<ClientResponse, HttpClientError>): Unit {
    match result {
        Ok(response) => print(response.status),
        Err(error) => print(match error.kind {
            InvalidRequest => "invalid", TimedOut => "timeout", TooLarge => "large", TooManyRedirects => "limit",
            RedirectRejected => "redirect rejected", Network => "network", _ => error.message(),
        }),
    }
}

async fn main(): Unit {
    val base = match args() { Ok(value) => value[0], Err(problem) => throw Exception(problem.message()) }
    val binary = require(await httpGet(base + "/data"))
    print(binary.text() == None && binary.body.size == 4 && binary.body[2] == 0 && binary.body[3] == 255)
    for value in binary.headerValues("sEt-CooKIE") { print(value.toString() ?? "invalid") }
    print(match binary.header("x-raw") { Some(value) => value.size == 1 && value[0] == 255, None => false })
    val notFound = require(await httpGet(base + "/missing"))
    print("${notFound.status}: ${notFound.text() ?? ""}")
    val payload = "A\0🙂".toBytes()
    val sent = require(await httpRequest("PUT", base + "/put", headers: [HttpHeader("x-echo", "token")], body: Some(payload)))
    print(sent.status == 201 && sent.body == payload)
    print(match sent.header("X-Echo") { Some(value) => value.toString() == Some("token"), None => false })
    show(await httpGet(base + "/redirect"))
    val follow = HttpClientOptions(maxRedirects: 1)
    show(await httpGet(base + "/redirect", follow))
    val head = require(await httpRequest("HEAD", base + "/redirect", options: follow))
    print(head.status == 200 && head.body.size == 0)
    show(await httpGet(base + "/redirect-elsewhere", follow))
    show(await httpGet(base + "/redirect-ambiguous", follow))
    show(await httpGet(base + "/redirect-loop", follow))
    show(await httpGet(base + "/large", HttpClientOptions(maxResponseBytes: 1024)))
    show(await httpGet(base + "/large-header", HttpClientOptions(maxResponseBytes: 128)))
    show(await httpGet(base + "/budget-start", HttpClientOptions(maxRedirects: 1, maxResponseBytes: 160)))
    show(await httpGet(base + "/slow", HttpClientOptions(timeoutMs: 150)))
    show(await httpGet("file:///not-a-network-resource"))
    show(await httpRequest("GET", base + "/data", headers: [HttpHeader("X-Bad", "a\r\nb")] ))
    show(await httpGet(base + "/data", HttpClientOptions(maxResponseBytes: 0)))
    val proxied = require(await httpGet("http://proxy-target.invalid/through", HttpClientOptions(proxy: Some(base))))
    print(proxied.text() ?? "invalid")
}
'''):
            with self.subTest(mode=app.stem), server() as fixture:
                direct = subprocess.run([app, f'http://127.0.0.1:{fixture.server_port}'], capture_output=True, timeout=15,
                                        env={**os.environ, 'http_proxy': 'http://127.0.0.1:1', 'NO_PROXY': '*'})
                self.assertEqual(direct.returncode, 0, direct.stderr.decode())
                self.assertEqual(direct.stderr, b'')
                self.assertEqual(direct.stdout, b'true\none=1\ntwo=2\ntrue\n404: not found\ntrue\ntrue\n302\n200\ntrue\n'
                                 b'redirect rejected\nnetwork\nlimit\nlarge\nlarge\nlarge\ntimeout\ninvalid\ninvalid\ninvalid\nexplicit proxy\n')
                self.assertTrue(fixture.arrived.is_set())

    @unittest.skipUnless(OPENSSL, 'local OpenSSL command is needed to generate an isolated test certificate')
    def test_https_validates_trust_and_hostname(self):
        certificate = self.root / 'server.pem'
        key = self.root / 'key.pem'
        config = self.root / 'openssl.cnf'
        config.write_text('''[req]
prompt = no
distinguished_name = dn
x509_extensions = extensions
[dn]
CN = localhost
[extensions]
basicConstraints = critical,CA:true
keyUsage = critical,digitalSignature,keyEncipherment,keyCertSign
subjectAltName = DNS:localhost
''', encoding='ascii')
        generate = subprocess.run([OPENSSL, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2', '-config', config.name,
                                   '-keyout', key.name, '-out', certificate.name], cwd=self.root, capture_output=True, timeout=15)
        self.assertEqual(generate.returncode, 0, generate.stderr.decode())
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(certificate, key)
        for app in self.programs('''
import std.httpClient.{ClientResponse}
import std.httpClient.{HttpClientError}
import std.httpClient.{HttpClientOptions}
import std.httpClient.{httpGet}
import std.io.{readBytes}
import std.os.{args}

fn show(result: Result<ClientResponse, HttpClientError>): Unit {
    match result {
        Ok(response) => print(response.text() ?? "unexpected bytes"),
        Err(error) => print(error.message()),
    }
}

async fn main(): Unit {
    val values = match args() { Ok(value) => value, Err(problem) => throw Exception(problem.message()) }
    val certificate = match readBytes(values[2]) { Ok(value) => value, Err(problem) => throw Exception(problem.message()) }
    show(await httpGet(values[0]))
    val options = HttpClientOptions(caPem: Some(certificate))
    show(await httpGet(values[0], options))
    show(await httpGet(values[1], options))
}
'''):
            with self.subTest(mode=app.stem), server(tls) as fixture:
                port = fixture.server_port
                result = subprocess.run([app, f'https://localhost:{port}/ok', f'https://127.0.0.1:{port}/ok', certificate],
                                        capture_output=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                failure = b'HTTP TLS connection or certificate verification failed\n'
                self.assertEqual((result.stdout, result.stderr), (failure + b'ok\n' + failure, b''))

    def test_cancel_waits_for_native_transfer_to_close(self):
        for app in self.programs('''
import std.httpClient.{HttpClientOptions}
import std.httpClient.{httpGet}
import std.io.{readLine}
import std.os.{args}

async fn main(): Unit {
    val base = match args() { Ok(value) => value[0], Err(problem) => throw Exception(problem.message()) }
    val pending = spawn httpGet(base + "/slow", HttpClientOptions(timeoutMs: 25000))
    val signal = await blocking { readLine() }
    match signal { Ok(Some("cancel")) => {}, _ => throw Exception("expected cancellation signal") }
    pending.cancel()
    match awaitOutcome(pending) {
        Cancelled(_) => print("cancelled"),
        _ => throw Exception("expected cancellation"),
    }
    match await httpGet(base + "/ok") {
        Ok(response) => print(response.text() ?? "invalid"),
        Err(problem) => throw Exception(problem.message()),
    }
}
'''):
            with self.subTest(mode=app.stem), server() as fixture:
                with running([app, f'http://127.0.0.1:{fixture.server_port}'], env={**os.environ, 'UV_THREADPOOL_SIZE': '2'}) as (process, output):
                    self.assertTrue(fixture.arrived.wait(timeout=8), 'native transfer did not reach the server')
                    process.stdin.write(b'cancel\n')
                    process.stdin.flush()
                    self.assertEqual(output.line(timeout=4), b'cancelled\n')
                    fixture.pending.settimeout(4)
                    try:
                        closing = fixture.pending.recv(1)
                    except ConnectionError:
                        closing = b''
                    self.assertEqual(closing, b'')
                    self.assertEqual(output.line(timeout=4), b'ok\n')
                    self.assertEqual(process.wait(timeout=4), 0)
                    self.assertEqual(process.stderr.read(), b'')

    def test_total_timeout_includes_waiting_for_a_native_worker(self):
        for app in self.programs('''
import std.httpClient.{HttpClientErrorKind}
import std.httpClient.{HttpClientOptions}
import std.httpClient.{httpGet}
import std.io.{readLine}
import std.os.{args}
import std.sync.{AtomicInt}

async fn main(): Unit {
    val entered = AtomicInt()
    val parked = spawn blocking {
        entered.store(1)
        val _ = readLine()
    }
    while entered.load() == 0 { await sleepMs(1) }
    val url = match args() { Ok(values) => values[0], Err(error) => throw Exception(error.message()) }
    val result = await httpGet(url, HttpClientOptions(timeoutMs: 100))
    #assert(match result { Err(error) => error.kind == HttpClientErrorKind.TimedOut, _ => false })
    print("timed out while queued")
    await parked
}
'''):
            with self.subTest(mode=app.stem), server() as fixture:
                with running([app, f'http://127.0.0.1:{fixture.server_port}/slow'],
                             env={**os.environ, 'UV_THREADPOOL_SIZE': '1'}) as (process, output):
                    self.assertEqual(output.line(timeout=5), b'timed out while queued\n')
                    process.stdin.write(b'release\n')
                    process.stdin.flush()
                    self.assertEqual(process.wait(timeout=5), 0, process.stderr.read().decode())
                    self.assertFalse(fixture.arrived.is_set())


if __name__ == '__main__':
    unittest.main()
