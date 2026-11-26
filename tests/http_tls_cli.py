import contextlib
import http.client
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import unittest

from platform_cli import assert_linkage, executable, running
from tls_fixture import OPENSSL, tls_fixture, trust_local


COMPILER = Path(sys.argv.pop(1)).resolve()


def fragmented_exchange(port, trust):
    incoming, outgoing = ssl.MemoryBIO(), ssl.MemoryBIO()
    client = trust.wrap_bio(incoming, outgoing, server_hostname='localhost')
    with socket.create_connection(('127.0.0.1', port), timeout=5) as connection:
        def flush():
            data = outgoing.read()
            for offset in range(0, len(data), 97):
                connection.sendall(data[offset:offset + 97])

        def run(operation):
            while True:
                try:
                    result = operation()
                    flush()
                    return result
                except ssl.SSLWantReadError:
                    flush()
                    data = connection.recv(16384)
                    if data:
                        incoming.write(data)
                    else:
                        incoming.write_eof()

        run(client.do_handshake)
        for fragment in (b'POST /echo HTTP/1.1\r\nHost: local', b'host\r\nConnection: close\r\nContent-Length: 4\r\n\r\n\x00', b'\xffAB'):
            run(lambda: client.write(fragment))
        clear = bytearray()
        while chunk := run(lambda: client.read(16384)):
            clear.extend(chunk)
        return bytes(clear)


@unittest.skipUnless(OPENSSL, 'OpenSSL is needed to generate an isolated local CA')
class HttpTls(unittest.TestCase):
    def test_tls_trust_handshake_deadline_and_disconnect(self):
        with tempfile.TemporaryDirectory(prefix='lyra-tls-') as temporary:
            root = Path(temporary)
            ca, chain, key = tls_fixture(root / '本地 CA')
            wrong_key = ca.with_name('ca.key')
            payload = root / 'payload.dat'
            expected_payload = bytes(range(256)) * 12288
            payload.write_bytes(expected_payload)
            source = root / 'src/main.ly'
            source.parent.mkdir()
            (root / 'lyra.json5').write_text('{name:"test/http-tls",version:"0.1.0"}', encoding='utf-8')
            source.write_text('''
import std.http.{Request}
import std.http.{Response, ResponseWriter}
import std.http.{Router}
import std.httpTls.{HttpServerOptions, listenTls}
import std.io.{readBytes}
import std.os.{args}

fn pem(path: String): Bytes {
    match readBytes(path) { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

async fn slow(request: Request): Response {
    defer print("cleaned")
    print("entered")
    await sleepMs(25000)
    Response.text(200, "not cancelled")
}

async fn healthy(request: Request): Response { Response.text(200, "ok") }

async fn main(): Int {
    val values = match args() { Ok(arguments) => arguments, Err(error) => throw Exception(error.message()) }
    val listener = match listenTls(0, pem(values[0]), pem(values[1]), options: HttpServerOptions(capacity: 2,
        handshakeTimeoutMs: 1000, streamRequestBody: true)) {
        Ok(value) => value,
        Err(error) => { print(error.message()); return 2 },
    }
    val router = Router()
    router.get("/slow", slow)
    router.get("/health", healthy)
    router.post("/echo", async (request: Request) => {
        #assert(request.bodyStreaming())
        val expected: List<UInt8> = [0, 255, 65, 66]
        var count = 0
        while true {
            match await request.readBody() {
                Ok(Some(chunk)) => {
                    for index in 0..<chunk.size { #assert(chunk[index] == expected[count]); count += 1 }
                },
                Ok(None) => break,
                Err(error) => throw Exception(error.message()),
            }
        }
        #assert(count == expected.size)
        Response.bytes(200, "application/octet-stream", b"\\x00\\xffAB")
    })
    router.get("/large", async (request: Request) => {
        val large = await blocking { pem(values[2]) }
        Response.bytes(200, "application/octet-stream", large)
    })
    router.get("/stream", async (request: Request) => {
        Response.stream(200, "text/plain", async (writer: ResponseWriter) => {
            (await writer.write("hello ".toBytes()))?
            await writer.write("TLS".toBytes())
        })
    })
    print(listener.port())
    await router.serve(listener)
    0
}
''', encoding='utf-8')
            for profile, options in (('debug', []), ('release', ['--release'])):
                with self.subTest(profile=profile):
                    app = executable(root / profile)
                    result = subprocess.run([COMPILER, 'build', root, *options, '-o', app], capture_output=True, timeout=45)
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    assert_linkage(self, app)
                    for arguments in ((chain, wrong_key), (key, key)):
                        result = subprocess.run([app, *arguments], capture_output=True, timeout=6)
                        self.assertEqual(result.returncode, 2, result.stderr.decode())
                        self.assertTrue(result.stdout.startswith(b'listenTls: invalid'), result.stdout)

                    with running([app, chain, key, payload]) as (process, output):
                        port = int(output.line())
                        trusted = trust_local(ca)
                        with contextlib.ExitStack() as clients:
                            peers = [clients.enter_context(socket.create_connection(('127.0.0.1', port), timeout=5)) for _ in range(4)]
                            peers[0].sendall(b'\x16\x03')
                            with trusted.wrap_socket(socket.create_connection(('127.0.0.1', port), timeout=5), server_hostname='localhost') as overloaded:
                                with contextlib.closing(http.client.HTTPResponse(overloaded)) as response:
                                    response.begin()
                                    self.assertEqual((response.status, response.getheader('Retry-After'), response.read()), (503, '1', b''))
                            for idle in peers:
                                try:
                                    self.assertEqual(idle.recv(1), b'')
                                except ConnectionResetError:
                                    pass

                        framed = fragmented_exchange(port, trusted)
                        framing, content = framed.split(b'\r\n\r\n', 1)
                        self.assertTrue(framing.startswith(b'HTTP/1.1 200 OK\r\n'))
                        self.assertEqual(content, b'\x00\xffAB')
                        with contextlib.closing(http.client.HTTPSConnection('localhost', port, context=trusted, timeout=5)) as stream:
                            stream.request('GET', '/large')
                            connection = stream.sock
                            response = stream.getresponse()
                            self.assertEqual((response.status, response.read()), (200, expected_payload))
                            stream.request('GET', '/stream')
                            response = stream.getresponse()
                            self.assertEqual((response.status, response.getheader('Transfer-Encoding'), response.read()), (200, 'chunked', b'hello TLS'))
                            for _ in range(3):
                                stream.request('GET', '/health')
                                response = stream.getresponse()
                                self.assertEqual((response.status, response.read()), (200, b'ok'))
                                self.assertIs(stream.sock, connection)
                        with trusted.wrap_socket(socket.create_connection(('127.0.0.1', port), timeout=5), server_hostname='localhost') as raw:
                            raw.sendall(b'GET /stream HTTP/1.0\r\nConnection: keep-alive\r\n\r\n')
                            response = http.client.HTTPResponse(raw)
                            response.begin()
                            self.assertEqual((response.status, response.getheader('Transfer-Encoding'), response.read()), (200, None, b'hello TLS'))

                        with self.assertRaises(ssl.SSLCertVerificationError), contextlib.closing(
                                http.client.HTTPSConnection('localhost', port, context=ssl.create_default_context(), timeout=5)) as untrusted:
                            untrusted.request('GET', '/health')
                        with self.assertRaises(ssl.SSLCertVerificationError), contextlib.closing(
                                http.client.HTTPSConnection('127.0.0.1', port, context=trusted, timeout=5)) as wrong_host:
                            wrong_host.request('GET', '/health')

                        for raw, status in (
                            (b'GET /health HTTP/1.1\r\n\r\n', 400),
                            (b'POST /health HTTP/1.1\r\nHost: localhost\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n', 417),
                        ):
                            with trusted.wrap_socket(socket.create_connection(('127.0.0.1', port), timeout=2), server_hostname='localhost') as connection:
                                connection.sendall(raw)
                                response = http.client.HTTPResponse(connection)
                                response.begin()
                                self.assertEqual((response.status, response.read()), (status, b''))

                        with trusted.wrap_socket(socket.create_connection(('127.0.0.1', port), timeout=5), server_hostname='localhost') as connection:
                            connection.sendall(b'GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n')
                            self.assertEqual(output.line(timeout=5), b'entered\n')
                        self.assertEqual(output.line(timeout=5), b'cleaned\n')
                        for version in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
                            client = trust_local(ca)
                            client.minimum_version = client.maximum_version = version
                            with contextlib.closing(http.client.HTTPSConnection('localhost', port, context=client, timeout=5)) as connection:
                                connection.request('GET', '/health')
                                response = connection.getresponse()
                                self.assertEqual((response.status, response.read()), (200, b'ok'))
                        self.assertIsNone(process.poll())


if __name__ == '__main__':
    unittest.main()
