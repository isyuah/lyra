import contextlib
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import unittest

from platform_cli import assert_linkage, executable, running
from tls_fixture import OPENSSL, tls_fixture, trust_local


COMPILER = Path(sys.argv.pop(1)).resolve()


def integer(value, prefix, high=0):
    limit = (1 << prefix) - 1
    if value < limit:
        return bytes([high | value])
    output = bytearray([high | limit])
    value -= limit
    while value >= 128:
        output.append((value & 127) | 128)
        value >>= 7
    output.append(value)
    return bytes(output)


def literal(value):
    encoded = value.encode('utf-8')
    return integer(len(encoded), 7) + encoded


class Http2Peer:
    def __init__(self, port, ca, response_window=65535):
        trust = trust_local(ca)
        trust.set_alpn_protocols(['h2', 'http/1.1'])
        self.socket = trust.wrap_socket(socket.create_connection(('127.0.0.1', port), timeout=6), server_hostname='localhost')
        self.socket.suppress_ragged_eofs = False
        if self.socket.selected_alpn_protocol() != 'h2':
            self.socket.close()
            raise AssertionError('TLS did not negotiate HTTP/2')
        self.socket.sendall(b'PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n')
        self.send(4, 0, 0, struct.pack('!HIHI', 2, 0, 4, response_window))
        self.bodies = {}
        self.ended = set()
        self.resets = {}
        self.windows = {}
        self.connection_window = 65535
        self.stream_window = 65535
        self.goaway = None
        self.frames = []
        self.settings = False
        self.settings_ack = False
        self.next_stream = 1
        self.response_window = response_window
        self.until(lambda: self.settings and self.settings_ack)

    def close(self):
        self.socket.close()

    def send(self, kind, flags, stream, payload=b''):
        self.socket.sendall(len(payload).to_bytes(3, 'big') + bytes([kind, flags]) + struct.pack('!I', stream) + payload)

    def read_exact(self, count):
        output = bytearray()
        while len(output) < count:
            chunk = self.socket.recv(count - len(output))
            if not chunk:
                raise EOFError('HTTP/2 connection ended before the expected frame')
            output.extend(chunk)
        return bytes(output)

    def receive(self):
        header = self.read_exact(9)
        size = int.from_bytes(header[:3], 'big')
        if size > 16384:
            raise AssertionError(f'peer exceeded the default frame size with {size} bytes')
        kind, flags = header[3:5]
        stream = int.from_bytes(header[5:], 'big') & 0x7fffffff
        payload = self.read_exact(size)
        self.frames.append((kind, flags, stream, payload))
        if kind == 4 and flags & 1:
            self.settings_ack = True
        elif kind == 4:
            for offset in range(0, size, 6):
                setting, value = struct.unpack('!HI', payload[offset:offset + 6])
                if setting == 4:
                    adjustment = value - self.stream_window
                    self.stream_window = value
                    for key in self.windows:
                        self.windows[key] += adjustment
            self.send(4, 1, 0)
            self.settings = True
        elif kind == 6 and not flags & 1:
            self.send(6, 1, 0, payload)
        elif kind == 8:
            credit = int.from_bytes(payload, 'big') & 0x7fffffff
            if stream:
                self.windows[stream] = self.windows.get(stream, self.stream_window) + credit
            else:
                self.connection_window += credit
        elif kind == 0:
            body = payload
            if flags & 8:
                body = body[1:len(body) - body[0]]
            self.bodies.setdefault(stream, bytearray()).extend(body)
            if size:
                self.send(8, 0, 0, struct.pack('!I', size))
                if self.response_window and not flags & 1:
                    self.send(8, 0, stream, struct.pack('!I', size))
        elif kind == 3:
            self.resets[stream] = int.from_bytes(payload, 'big')
        elif kind == 7:
            self.goaway = struct.unpack('!II', payload[:8])
        if kind in (0, 1) and flags & 1:
            self.ended.add(stream)
        return kind, flags, stream, payload

    def until(self, complete):
        for _ in range(10000):
            if complete():
                return
            self.receive()
        raise AssertionError('HTTP/2 exchange exceeded its frame budget')

    def request(self, path, method='GET', fields=(), end=True):
        stream = self.next_stream
        self.next_stream += 2
        block = (b'\x82' if method == 'GET' else b'\x83' if method == 'POST' else b'\x02' + literal(method))
        block += b'\x87\x04' + literal(path) + b'\x01' + literal('localhost')
        for name, value in fields:
            block += b'\x00' + literal(name) + literal(value)
        self.windows[stream] = self.stream_window
        self.send(1, 4 | (1 if end else 0), stream, block)
        return stream

    def data(self, stream, payload, end=False):
        offset = 0
        while offset < len(payload):
            self.until(lambda: min(self.connection_window, self.windows[stream]) > 0 or stream in self.resets)
            if stream in self.resets:
                raise AssertionError(f'upload was reset with HTTP/2 error {self.resets[stream]}')
            count = min(16384, len(payload) - offset, self.connection_window, self.windows[stream])
            self.connection_window -= count
            self.windows[stream] -= count
            final = end and offset + count == len(payload)
            self.send(0, 1 if final else 0, stream, payload[offset:offset + count])
            offset += count
        if end and not payload:
            self.send(0, 1, stream)

    def body(self, stream):
        self.until(lambda: stream in self.ended or stream in self.resets)
        if stream not in self.ended:
            raise AssertionError(f'response stream {stream} reset with error {self.resets[stream]}')
        return bytes(self.bodies.get(stream, b''))


@unittest.skipUnless(OPENSSL, 'OpenSSL is needed to generate an isolated local CA')
class Http2(unittest.TestCase):
    def test_alpn_multiplexing_flow_control_reset_and_drain(self):
        with tempfile.TemporaryDirectory(prefix='lyra-http2-') as temporary:
            root = Path(temporary)
            ca, chain, key = tls_fixture(root / '本地 CA')
            source = root / 'src/main.ly'
            source.parent.mkdir()
            (root / 'lyra.json5').write_text('{name:"test/http2",version:"0.1.0"}', encoding='utf-8')
            source.write_text('''
import std.http.{Request, Response, ResponseWriter, Router, Shutdown}
import std.httpTls.{HttpServerOptions, listenTls}
import std.httpClient.{ClientResponse, HttpClientOptions, httpRequest}
import std.io.{readBytes}
import std.os.{args}

fn pem(path: String): Bytes {
    match readBytes(path) { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

async fn fetch(method: String, url: String, options: HttpClientOptions): ClientResponse {
    match await httpRequest(method, url, options: options) {
        Ok(response) => response, Err(error) => throw Exception(error.message()),
    }
}

async fn checkHeaders(ca: String, port: String): Unit {
    val options = HttpClientOptions(caPem: pem(ca))
    val base = "https://localhost:${port}"
    val response = await fetch("GET", base + "/headers", options)
    #assert(response.status == 200 && response.body == b"payload")
    #assert(response.header("x-mixed") == Some(b"yes"))
    #assert(response.header("content-type") == Some(b"application/x-custom; charset=utf-8"))
    val cookies = response.headerValues("Set-Cookie")
    #assert(cookies.size == 2 && cookies[0] == b"a=1" && cookies[1] == b"b=2")
    for header in response.headers { #assert(header.name != "X-MiXeD") }
    val head = await fetch("HEAD", base + "/headers", options)
    #assert(head.status == 200 && head.body.size == 0)
    #assert(head.header("content-length") == Some(b"7"))
    val empty = await fetch("GET", base + "/empty", options)
    #assert(empty.status == 204 && empty.body.size == 0)
    val cached = await fetch("GET", base + "/cached", options)
    #assert(cached.status == 304 && cached.body.size == 0)
    val binary = await fetch("GET", base + "/binary", options)
    #assert(binary.status == 200 && binary.body == b"\\x00\\xffAB")
    #assert(binary.header("content-type") == Some(b"application/octet-stream"))
    print("client-headers-ok")
}

async fn main(): Int {
    val values = match args() { Ok(value) => value, Err(error) => throw Exception(error.message()) }
    if values[0] == "client" { await checkHeaders(values[1], values[2]); return 0 }
    val mode = if values.size > 2 { values[2] } else { "default" }
    val listener = match listenTls(0, pem(values[0]), pem(values[1]), options: HttpServerOptions(capacity: 8,
        streamRequestBody: true, maxBodyBytes: 1048576, maxRequestBytes: 2097152,
        graceMs: if mode == "grace" { 100 } else { 1000 },
        enableHttp2: mode != "http1", keepAliveTimeoutMs: if mode == "no-keepalive" { 0 } else { 5000 },
        maxRequestsPerConnection: if mode == "one-request" { 1 } else { 1000 })) {
        Ok(value) => value, Err(error) => throw Exception(error.message()),
    }
    val shutdown = Shutdown()
    val router = Router()
    router.get("/health", async (request: Request) => { Response.text(200, "healthy") })
    router.get("/headers", async (request: Request) => {
        Response.bytes(200, "application/x-custom; charset=utf-8", b"payload").withHeader("X-MiXeD", "yes")
            .withHeader("Set-Cookie", "a=1").withHeader("set-cookie", "b=2")
    })
    router.get("/empty", async (request: Request) => { Response.text(204, "") })
    router.get("/cached", async (request: Request) => { Response.text(304, "") })
    router.get("/binary", async (request: Request) => { Response.bytes(200, "application/octet-stream", b"\\x00\\xffAB") })
    router.get("/slow", async (request: Request) => {
        defer print("slow-cleaned")
        print("slow-entered")
        await sleepMs(30000)
        Response.text(200, "slow")
    })
    router.post("/upload", async (request: Request) => {
        var total = 0
        while true {
            match await request.readBody() {
                Ok(Some(bytes)) => { total += bytes.size },
                Ok(None) => break,
                Err(error) => throw Exception(error.message()),
            }
        }
        Response.text(200, "${total}")
    })
    router.post("/blocked", async (request: Request) => {
        defer print("blocked-cleaned")
        print("blocked-entered")
        await sleepMs(30000)
        Response.text(200, "blocked")
    })
    router.post("/early", async (request: Request) => { Response.text(200, "finished-early") })
    router.get("/stream", async (request: Request) => {
        Response.stream(200, "text/plain", async (writer: ResponseWriter) => {
            for index in 0..<40 { (await writer.write("0123456789".toBytes()))? }
            Ok({})
        })
    })
    router.get("/stop", async (request: Request) => { shutdown.request(); Response.text(200, "stopping") })
    print(listener.port())
    await router.serve(listener, shutdown)
    print("stopped")
    0
}
''', encoding='utf-8')
            for profile, options in (('debug', []), ('release', ['--release'])):
                with self.subTest(profile=profile):
                    app = executable(root / profile)
                    result = subprocess.run([COMPILER, 'build', root, *options, '-o', app], capture_output=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    assert_linkage(self, app)
                    with running([app, chain, key]) as (process, output):
                        port = int(output.line())
                        with contextlib.closing(Http2Peer(port, ca)) as peer:
                            slow = peer.request('/slow')
                            self.assertEqual(output.line(), b'slow-entered\n')
                            healthy = peer.request('/health')
                            self.assertEqual(peer.body(healthy), b'healthy')
                            self.assertNotIn(slow, peer.ended)
                            peer.send(3, 0, slow, struct.pack('!I', 8))
                            self.assertEqual(output.line(), b'slow-cleaned\n')
                            healthy = peer.request('/health')
                            self.assertEqual(peer.body(healthy), b'healthy')
                            streamed = peer.request('/stream')
                            self.assertEqual(peer.body(streamed), b'0123456789' * 40)

                            blocked = peer.request('/blocked', 'POST', end=False)
                            self.assertEqual(output.line(), b'blocked-entered\n')
                            peer.data(blocked, b'x' * 65535)
                            healthy = peer.request('/health')
                            self.assertEqual(peer.body(healthy), b'healthy')
                            self.assertEqual(peer.windows[blocked], 0)
                            peer.send(3, 0, blocked, struct.pack('!I', 8))
                            self.assertEqual(output.line(), b'blocked-cleaned\n')

                            payload = bytes(range(256)) * 2048
                            upload = peer.request('/upload', 'POST', [('content-length', str(len(payload)))], end=False)
                            peer.data(upload, payload, end=True)
                            self.assertEqual(peer.body(upload), str(len(payload)).encode())
                            early = peer.request('/early', 'POST', end=False)
                            self.assertEqual(peer.body(early), b'finished-early')
                            peer.until(lambda: early in peer.resets)
                            self.assertEqual(peer.resets[early], 0)
                            malformed = peer.request('/health', fields=[('connection', 'close')])
                            peer.until(lambda: malformed in peer.resets or malformed in peer.ended)
                            healthy = peer.request('/health')
                            self.assertEqual(peer.body(healthy), b'healthy')
                            for path, method in (('/headers', 'HEAD'), ('/empty', 'GET'), ('/cached', 'GET')):
                                empty = peer.request(path, method)
                                self.assertEqual(peer.body(empty), b'')
                                self.assertFalse(any(kind == 0 and stream == empty for kind, _, stream, _ in peer.frames))

                        client = subprocess.run([app, 'client', ca, str(port)], capture_output=True, timeout=20)
                        self.assertEqual(client.returncode, 0, client.stderr.decode())
                        self.assertEqual(client.stdout, b'client-headers-ok\n')

                        with contextlib.closing(Http2Peer(port, ca)) as peer:
                            healthy = peer.request('/health')
                            peer.send(7, 0, 0, struct.pack('!II', 0, 0))
                            self.assertEqual(peer.body(healthy), b'healthy')
                            peer.until(lambda: peer.goaway is not None)
                            self.assertEqual(peer.goaway, (healthy, 0))

                        with contextlib.closing(Http2Peer(port, ca, response_window=0)) as peer:
                            stalled = peer.request('/health')
                            peer.until(lambda: any(kind == 1 and stream == stalled for kind, _, stream, _ in peer.frames))
                            self.assertNotIn(stalled, peer.ended)
                            self.assertNotIn(stalled, peer.bodies)
                            healthy = peer.request('/health')
                            peer.send(8, 0, healthy, struct.pack('!I', 7))
                            self.assertEqual(peer.body(healthy), b'healthy')
                            self.assertNotIn(stalled, peer.ended)
                            peer.send(8, 0, stalled, struct.pack('!I', 7))
                            self.assertEqual(peer.body(stalled), b'healthy')

                        fallback = trust_local(ca)
                        fallback.set_alpn_protocols(['http/1.1'])
                        with fallback.wrap_socket(socket.create_connection(('127.0.0.1', port), timeout=6), server_hostname='localhost') as connection:
                            connection.sendall(b'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
                            response = bytearray()
                            while chunk := connection.recv(16384):
                                response.extend(chunk)
                            self.assertTrue(response.startswith(b'HTTP/1.1 200 '), response)
                            self.assertTrue(response.endswith(b'healthy'), response)

                        with contextlib.closing(Http2Peer(port, ca)) as peer:
                            stopped = peer.request('/stop')
                            self.assertEqual(peer.body(stopped), b'stopping')
                            peer.until(lambda: peer.goaway is not None)
                            self.assertEqual(peer.goaway[1], 0)
                        self.assertEqual(output.line(), b'stopped\n')
                        self.assertEqual(process.wait(timeout=6), 0)

                    with running([app, chain, key, 'http1']) as (_, output):
                        port = int(output.line())
                        http1 = trust_local(ca)
                        http1.set_alpn_protocols(['h2', 'http/1.1'])
                        with http1.wrap_socket(socket.create_connection(('127.0.0.1', port), timeout=6), server_hostname='localhost') as connection:
                            self.assertEqual(connection.selected_alpn_protocol(), 'http/1.1')
                            connection.sendall(b'GET /stop HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
                            self.assertEqual(output.line(), b'stopped\n')

                    for mode in ('no-keepalive', 'one-request'):
                        with self.subTest(mode=mode), running([app, chain, key, mode]) as (_, output):
                            port = int(output.line())
                            with contextlib.closing(Http2Peer(port, ca)) as peer:
                                streamed = peer.request('/stream')
                                self.assertEqual(peer.body(streamed), b'0123456789' * 40)
                                peer.until(lambda: peer.goaway is not None)
                                self.assertEqual(peer.goaway, (streamed, 0))

                    for attempt in range(3):
                        with self.subTest(shutdown=attempt), running([app, chain, key]) as (process, output):
                            port = int(output.line())
                            with contextlib.closing(Http2Peer(port, ca)) as peer:
                                stopped = peer.request('/stop')
                                self.assertEqual(peer.body(stopped), b'stopping')
                                peer.until(lambda: peer.goaway is not None)
                                self.assertEqual(peer.goaway, (stopped, 0))
                                self.assertEqual(peer.socket.recv(1), b'')
                            self.assertEqual(output.line(), b'stopped\n')
                            self.assertEqual(process.wait(timeout=6), 0)

                    with running([app, chain, key, 'grace']) as (process, output):
                        port = int(output.line())
                        with contextlib.closing(Http2Peer(port, ca)) as peer:
                            slow = peer.request('/slow')
                            self.assertEqual(output.line(), b'slow-entered\n')
                            stopped = peer.request('/stop')
                            self.assertEqual(peer.body(stopped), b'stopping')
                            peer.until(lambda: peer.goaway is not None)
                            self.assertEqual(peer.goaway, (stopped, 0))
                            self.assertNotIn(slow, peer.ended)
                            self.assertEqual(output.line(), b'slow-cleaned\n')
                            self.assertEqual(output.line(), b'stopped\n')
                            self.assertEqual(process.wait(timeout=6), 0)


if __name__ == '__main__':
    unittest.main()
