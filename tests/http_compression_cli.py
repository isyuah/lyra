import gzip
import http.client
from pathlib import Path
import random
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

from platform_cli import WINDOWS, assert_linkage, executable, running


COMPILER = Path(sys.argv.pop(1)).resolve()

SOURCE = r'''
import std.http.{HttpError, HttpErrorKind, HttpServerOptions, listen, Request, Response, ResponseWriter, Router}
import std.httpCompression.{gzipResponse, GzipRequestReader}
import std.io.{IoError, readBytes}
import std.sync.{AtomicInt, Locked}

fn requireHttp<T>(result: Result<T, HttpError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

fn requireIo<T>(result: Result<T, IoError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

async fn echo(request: Request): Response {
    val reader = match GzipRequestReader.fromRequest(request, maxOutputBytes: 196608) {
        Ok(value) => value, Err(_) => { return Response.text(415, "expected gzip") },
    }
    defer { val _ = reader.close() }
    var count = 0
    var checksum = 0
    while true {
        match await reader.read() {
            Ok(Some(content)) => {
                #assert(content.size > 0 && content.size <= 16384)
                count += content.size
                for byte in content { checksum += byte }
            },
            Ok(None) => { break },
            Err(error) => {
                val repeated = await reader.read()
                #assert(match repeated { Err(next) => next.kind == error.kind, _ => false })
                return match error.kind {
                    TooLarge => Response.text(413, "too large"),
                    Malformed => Response.text(400, "bad gzip"),
                    _ => Response.text(500, error.message()),
                }
            },
        }
    }
    #assert(requireHttp(await reader.read()) == None)
    Response.text(200, "${count} ${checksum}")
}

async fn main(): Unit {
    val listener = requireHttp(listen(0, options: HttpServerOptions(streamRequestBody: true)))
    val router = Router()
    val large = requireIo(readBytes("原始 🙂.bin"))
    val runs = AtomicInt()
    val retained: Locked<ResponseWriter?> = Locked(None)
    router.get("/buffered", async (request: Request) => {
        val response = Response.bytes(200, "application/octet-stream", large)
            .withHeader("Vary", "Origin").withHeader("vary", "ACCEPT-ENCODING")
            .withHeader("ETag", "\"identity\"").withHeader("Content-Digest", "sha-256=:old:")
            .withHeader("Accept-Ranges", "bytes")
        requireHttp(gzipResponse(request, response))
    })
    router.get("/stream", async (request: Request) => {
        val response = Response.stream(200, "application/octet-stream", async (writer: ResponseWriter) => {
            val _ = runs.fetchAdd(1)
            retained.update { _ => Some(writer) }
            (await writer.write(b"first\0\xff"))?
            print("first")
            await writer.write(large)
        })
        requireHttp(gzipResponse(request, response))
    })
    router.get("/state", async (_: Request) => {
        if let Some(old) = retained.snapshot() {
            val late = await old.write(b"late")
            #assert(match late { Err(error) => error.kind == HttpErrorKind.Closed, _ => false })
        }
        Response.text(200, "${runs.load()}")
    })
    router.get("/star", async (request: Request) => {
        requireHttp(gzipResponse(request, Response.text(200, "content").withHeader("Vary", "*")))
    })
    router.get("/partial", async (request: Request) => {
        requireHttp(gzipResponse(request, Response.text(206, "part").withHeader("Content-Range", "bytes 0-3/10")))
    })
    router.get("/forbidden-status", async (request: Request) => {
        requireHttp(gzipResponse(request, Response.text(204, "")))
    })
    router.get("/encoded", async (request: Request) => {
        requireHttp(gzipResponse(request, Response.text(200, "content").withHeader("content-encoding", "identity")))
    })
    router.get("/no-transform", async (request: Request) => {
        requireHttp(gzipResponse(request, Response.text(200, "content").withHeader("Cache-Control", "max-age=0, NO-TRANSFORM")))
    })
    router.get("/quoted-directive", async (request: Request) => {
        requireHttp(gzipResponse(request, Response.text(200, "content").withHeader("Cache-Control", "other=\"example, no-transform, next\"")))
    })
    router.get("/failure", async (request: Request) => {
        requireHttp(gzipResponse(request, Response.stream(200, "text/plain", async (writer: ResponseWriter) => {
            retained.update { _ => Some(writer) }
            (await writer.write(b"first"))?
            Err(HttpError(HttpErrorKind.Invalid, "producer"))
        })))
    })
    router.get("/throw", async (request: Request) => {
        requireHttp(gzipResponse(request, Response.stream(200, "text/plain", async (writer: ResponseWriter) => {
            retained.update { _ => Some(writer) }
            (await writer.write(b"first"))?
            throw Exception("producer failed")
        })))
    })
    router.get("/cancel", async (request: Request) => {
        requireHttp(gzipResponse(request, Response.stream(200, "text/plain", async (writer: ResponseWriter) => {
            defer print("producer stopped")
            retained.update { _ => Some(writer) }
            (await writer.write(b"first"))?
            await sleepMs(60000)
            Ok({})
        })))
    })
    router.post("/decode", echo)
    print(listener.port())
    await router.serve(listener)
}
'''


def exchange(connection, path, headers=(), body=None, method='GET', chunked=False):
    connection.putrequest(method, path, skip_accept_encoding=True)
    for name, value in headers:
        connection.putheader(name, value)
    connection.endheaders(body, encode_chunked=chunked)
    response = connection.getresponse()
    return response.status, response.getheaders(), response.read()


def field_values(fields, name):
    return [value for field, value in fields if field.lower() == name.lower()]


class HttpCompression(unittest.TestCase):
    def test_negotiation_streaming_and_validated_decompression(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'src').mkdir()
            (root / 'src/main.ly').write_text(SOURCE, encoding='utf-8')
            (root / 'lyra.json5').write_text('{name:"test/http-compression",version:"0.1.0"}', encoding='utf-8')
            app = executable(root / 'server')
            payload = random.Random(947).randbytes(180000)
            (root / '原始 🙂.bin').write_bytes(payload)
            result = subprocess.run([COMPILER, 'build', root, '-o', app], capture_output=True, timeout=45)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            assert_linkage(self, app)

            with running([app], cwd=root) as (_, output):
                port = int(output.line())
                connection = http.client.HTTPConnection('127.0.0.1', port, timeout=8)
                self.addCleanup(connection.close)
                encoding = 'Accept-Encoding'
                supported = [(encoding, 'GZip; q=0.300'), (encoding, 'br, identity;q=0.2')]
                status, fields, body = exchange(connection, '/buffered', supported)
                self.assertEqual((status, gzip.decompress(body)), (200, payload))
                self.assertEqual(field_values(fields, 'Content-Encoding'), ['gzip'])
                self.assertEqual(field_values(fields, 'Vary'), ['Origin', 'ACCEPT-ENCODING', 'Range, Cache-Control'])
                for name in ['ETag', 'Content-Digest', 'Accept-Ranges']:
                    self.assertFalse(field_values(fields, name))

                cases = [
                    ([], None, 200),
                    ([(encoding, '')], None, 200),
                    ([(encoding, 'br, *;q=0.050')], 'gzip', 200),
                    ([(encoding, 'gzip;q=0, *;q=1')], None, 200),
                    ([(encoding, 'gzip;q=1'), (encoding, 'gzip;q=0')], None, 200),
                    ([(encoding, 'gzip;q=0.2, identity;q=0.3')], None, 200),
                    ([(encoding, 'gzip;q=1.001')], None, 200),
                    ([(encoding, 'gzip;q=0.0001')], None, 200),
                    ([(encoding, 'gzip, not a coding')], None, 200),
                    ([(encoding, 'identity;q=0, gzip;q=0')], None, 406),
                    ([(encoding, '*;q=0')], None, 406),
                    ([(encoding, 'gzip'), ('Range', 'bytes=0-1')], None, 200),
                    ([(encoding, 'gzip'), ('Cache-Control', 'max-age=0, No-Transform')], None, 200),
                ]
                for headers, coding, expected in cases:
                    with self.subTest(headers=headers):
                        status, fields, body = exchange(connection, '/buffered', headers)
                        self.assertEqual(status, expected)
                        self.assertEqual(field_values(fields, 'Content-Encoding'), [coding] if coding else [])
                        if status == 200:
                            self.assertEqual(gzip.decompress(body) if coding else body, payload)
                            if not coding:
                                self.assertEqual(field_values(fields, 'ETag'), ['"identity"'])

                gzip_only = [(encoding, 'gzip')]
                for path, expected, content_coding, content in (
                    ('/partial', 206, [], b'part'), ('/forbidden-status', 204, [], b''),
                    ('/encoded', 200, ['identity'], b'content'), ('/no-transform', 200, [], b'content'),
                ):
                    status, fields, body = exchange(connection, path, gzip_only)
                    self.assertEqual((status, field_values(fields, 'Content-Encoding'), body), (expected, content_coding, content))
                status, fields, body = exchange(connection, '/star', gzip_only)
                self.assertEqual((status, field_values(fields, 'Vary'), gzip.decompress(body)), (200, ['*'], b'content'))
                status, fields, body = exchange(connection, '/quoted-directive', gzip_only)
                self.assertEqual((status, field_values(fields, 'Content-Encoding'), gzip.decompress(body)), (200, ['gzip'], b'content'))

                status, fields, body = exchange(connection, '/stream', gzip_only, method='HEAD')
                self.assertEqual((status, field_values(fields, 'Content-Encoding'), body), (200, ['gzip'], b''))
                self.assertEqual(exchange(connection, '/state')[2], b'0')
                connection.putrequest('GET', '/stream', skip_accept_encoding=True)
                connection.putheader(encoding, 'gzip')
                connection.endheaders()
                streamed = connection.getresponse()
                self.assertEqual(output.line().strip(), b'first')
                decoder = zlib.decompressobj(wbits=31)
                prefix = b''
                while len(prefix) < 7:
                    wire = streamed.read(1)
                    self.assertTrue(wire)
                    prefix += decoder.decompress(wire)
                self.assertEqual(prefix, b'first\0\xff')
                self.assertFalse(decoder.eof)
                self.assertEqual(decoder.decompress(streamed.read()) + decoder.flush(), payload)
                self.assertTrue(decoder.eof)
                self.assertEqual(exchange(connection, '/state')[2], b'1')

                for path in ['/failure', '/throw']:
                    failing = http.client.HTTPConnection('127.0.0.1', port, timeout=8)
                    try:
                        failing.putrequest('GET', path, skip_accept_encoding=True)
                        failing.putheader(encoding, 'gzip')
                        failing.endheaders()
                        response = failing.getresponse()
                        with self.assertRaises(http.client.IncompleteRead) as incomplete:
                            response.read()
                        partial = zlib.decompressobj(wbits=31)
                        self.assertEqual(partial.decompress(incomplete.exception.partial), b'first')
                        self.assertFalse(partial.eof)
                    finally:
                        failing.close()
                    self.assertEqual(exchange(connection, '/state')[2], b'1')

                cancelled = http.client.HTTPConnection('127.0.0.1', port, timeout=8)
                cancelled.putrequest('GET', '/cancel', skip_accept_encoding=True)
                cancelled.putheader(encoding, 'gzip')
                cancelled.endheaders()
                response = cancelled.getresponse()
                partial = zlib.decompressobj(wbits=31)
                first = b''
                while len(first) < 5:
                    first += partial.decompress(response.read(1))
                self.assertEqual(first, b'first')
                cancelled.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('HH' if WINDOWS else 'ii', 1, 0))
                response.close()
                cancelled.close()
                self.assertEqual(output.line().strip(), b'producer stopped')
                self.assertEqual(exchange(connection, '/state')[2], b'1')

                compressed = gzip.compress(payload, mtime=0) + gzip.compress(b'\x00\xff', mtime=0)
                items = (compressed[index:index + 5] for index in range(0, len(compressed), 5))
                sock = connection.sock
                status, _, body = exchange(connection, '/decode', [('Content-Encoding', 'GZIP'), ('Transfer-Encoding', 'chunked')],
                                           items, method='POST', chunked=True)
                self.assertEqual((status, body), (200, f'{len(payload) + 2} {sum(payload) + 255}'.encode()))
                self.assertIs(connection.sock, sock)

                for compressed, expected in [
                    (gzip.compress(b'x' * 196609), 413),
                    (gzip.compress(b'payload')[:-1], 400),
                    (gzip.compress(b'payload') + b'invalid', 400),
                ]:
                    status, _, _ = exchange(connection, '/decode', [('Content-Encoding', 'gzip'), ('Content-Length', str(len(compressed)))],
                                            compressed, method='POST')
                    self.assertEqual(status, expected)
                status, _, _ = exchange(connection, '/decode', [('Content-Encoding', 'identity'), ('Content-Length', '0')], method='POST')
                self.assertEqual(status, 415)
                self.assertEqual(exchange(connection, '/state')[2], b'1')


if __name__ == '__main__':
    unittest.main()
