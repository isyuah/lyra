import contextlib
import http.client
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest

from platform_cli import executable, running


COMPILER = Path(sys.argv.pop(1)).resolve()
BOUNDARY = b'test: upload'
MEDIA = 'Multipart/Form-Data; ignored=token; BOUNDARY="test: upload"'

SOURCE = r'''
import std.http.{HttpError, HttpServerOptions, Request, Response, Router, listen}
import std.httpMultipart.{MultipartError, MultipartErrorKind, MultipartLimit, MultipartOptions, MultipartReader}
import std.io.{File, IoError, readLine}
import std.os.{args}

fn requireHttp<T>(result: Result<T, HttpError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

fn requireIo<T>(result: Result<T, IoError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

fn requirePart<T>(result: Result<T, MultipartError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

fn multipartFailed<T>(result: Result<T, MultipartError>, expected: MultipartErrorKind): Bool {
    match result { Ok(_) => false, Err(error) => error.kind == expected }
}

async fn inspect(request: Request, destination: String): Result<String, MultipartError> {
    val gated = request.target() == "/gated"
    val options = MultipartOptions(maxParts: 2, maxPartHeaderBytes: 256, maxFieldBytes: 16,
        maxFileBytes: if request.target() == "/tiny" { 4 } else { 3 * 1024 * 1024 },
        maxTotalBytes: if request.target() == "/total" { 128 } else { 4 * 1024 * 1024 })
    val reader = MultipartReader.fromRequest(request, options)?
    var summary = ""
    while true {
        val current = (await reader.next())?
        if let Some(part) = current {
            #assert(multipartFailed(await reader.next(), MultipartErrorKind.PartPending))
            if let Some(filename) = part.filename {
                val output = requireIo(File.openWrite(destination))
                defer output.close()
                var size = 0
                if gated {
                    #assert(part.name == "内容")
                    #assert(filename == "../a\".bin")
                    #assert(part.contentType == Some("application/octet-stream"))
                    #assert(part.headers.size == 4)
                    #assert(part.headers[2].name == "X-Tag" && part.headers[2].value == "first")
                    #assert(part.headers[3].name == "x-tag" && part.headers[3].value == "second")
                    val reading = spawn part.read()
                    await sleepMs(0)
                    #assert(multipartFailed(await reader.next(), MultipartErrorKind.Busy))
                    #assert(multipartFailed(await part.skip(), MultipartErrorKind.Busy))
                    print("part ready")
                    val first = (await reading)?
                    #assert(first == Some(b"F"))
                    requireIo(output.write(b"F"))
                    size += 1
                    print("first delivered")
                }
                while true {
                    match (await part.read())? {
                        Some(bytes) => { requireIo(output.write(bytes)); size += bytes.size },
                        None => break,
                    }
                }
                summary += "${part.name}:${size};"
            } else if part.name == "skip" {
                (await part.skip())?
                summary += "skipped;"
            } else {
                var text = ""
                while true {
                    match (await part.read())? {
                        Some(bytes) => { text += bytes.toString() ?? throw Exception("expected ASCII fixture") },
                        None => break,
                    }
                }
                summary += "${part.name}:${text};"
            }
            #assert(requirePart(await part.read()) == None)
        } else { break }
    }
    #assert(match await reader.next() { Ok(None) => true, _ => false })
    Ok(summary)
}

fn failure(error: MultipartError): Response {
    match error.kind {
        TooLarge(limit) => Response.text(413, match limit {
            TotalBytes => "total", PartCount => "parts", PartHeaders => "headers", FieldBytes => "field", FileBytes => "file",
        }),
        StreamingRequired => Response.text(415, "buffered"),
        ContentType => Response.text(415, "media"),
        Truncated => Response.text(400, "truncated"),
        Malformed => Response.text(400, "malformed"),
        _ => Response.text(400, error.message()),
    }
}

async fn expirePart(request: Request): Response {
    val reader = requirePart(MultipartReader.fromRequest(request))
    val first = requirePart(await reader.next()) ?? throw Exception("first part missing")
    requirePart(await first.skip())
    val second = requirePart(await reader.next()) ?? throw Exception("second part missing")
    #assert(first.name == "first" && second.name == "second")
    #assert(multipartFailed(await first.read(), MultipartErrorKind.Closed))
    #assert(multipartFailed(await first.skip(), MultipartErrorKind.Closed))
    requirePart(await second.skip())
    #assert(match await reader.next() { Ok(None) => true, _ => false })
    #assert(multipartFailed(await second.read(), MultipartErrorKind.Closed))
    Response.text(200, "stale checked")
}

async fn cancelRead(request: Request): Response {
    val reader = requirePart(MultipartReader.fromRequest(request))
    val reading = spawn reader.next()
    await sleepMs(0)
    #assert(multipartFailed(await reader.next(), MultipartErrorKind.Busy))
    print("waiting for cancellation")
    await blocking { val _ = readLine() }
    reading.cancel()
    #assert(match awaitOutcome(reading) { Cancelled(_) => true, _ => false })
    #assert(match await reader.next() { Err(error) => match error.kind { Http(_) => true, _ => false }, _ => false })
    print("lease released")
    Response.text(200, "must not write")
}

async fn main(): Unit {
    val arguments = requireIo(args())
    val destination = arguments[0]
    val stream = arguments.size == 1
    val listener = requireHttp(listen(0, options: HttpServerOptions(capacity: 2, requestTimeoutMs: 20000,
        maxBodyBytes: 5242880, maxRequestBytes: 10485760, streamRequestBody: stream)))
    val router = Router()
    for path in ["/upload", "/gated", "/tiny", "/total"] {
        router.post(path, async (request: Request) => {
            match await inspect(request, destination) { Ok(summary) => Response.text(200, summary), Err(error) => failure(error) }
        })
    }
    router.post("/stale", expirePart)
    router.post("/cancel", cancelRead)
    router.get("/ok", async (request: Request) => { Response.text(200, "ok") })
    print(listener.port())
    await router.serve(listener)
}
'''


def header(name, filename=None, extra=()):
    disposition = b'Content-Disposition: form-data; name="' + name + b'"'
    if filename is not None:
        disposition += b'; filename="' + filename + b'"'
    return b'\r\n'.join([disposition, *extra]) + b'\r\n\r\n'


def multipart(*parts):
    content = b''.join(b'--' + BOUNDARY + b'\r\n' + head + body + b'\r\n' for head, body in parts)
    return content + b'--' + BOUNDARY + b'--\r\n'


def response(connection):
    with connection.getresponse() as reply:
        return reply.status, reply.read()


def upload(connection, payload, path='/upload', media=MEDIA):
    connection.request('POST', path, body=payload, headers={} if media is None else {'Content-Type': media},
                       encode_chunked=not isinstance(payload, bytes))
    return response(connection)


@contextlib.contextmanager
def server(app, destination, *flags):
    with running([app, destination, *flags]) as (process, output):
        line = output.line()
        if not line:
            raise RuntimeError(f"server exited ({process.wait(timeout=5)}): {process.stderr.read().decode(errors='replace')}")
        yield int(line), process, output


class HttpMultipart(unittest.TestCase):
    def test_streaming_metadata_boundaries_limits_and_cancellation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / '表单 上传'
            (root / 'src').mkdir(parents=True)
            (root / 'lyra.json5').write_text('{name:"test/http-multipart",version:"1.0.0"}', encoding='utf-8')
            (root / 'src/main.ly').write_text(SOURCE, encoding='utf-8')
            for profile, flags in (('debug', []), ('release', ['--release'])):
                with self.subTest(profile=profile):
                    app = executable(root / profile)
                    result = subprocess.run([COMPILER, 'build', root, *flags, '-o', app], capture_output=True, timeout=60)
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    self.check_streaming(app, root / 'trusted.bin')
                    with server(app, root / 'trusted.bin', 'buffered') as (port, _, _):
                        with contextlib.closing(http.client.HTTPConnection('127.0.0.1', port, timeout=5)) as connection:
                            self.assertEqual(upload(connection, multipart((header(b'field'), b'hello'))), (415, b'buffered'))

    def check_streaming(self, app, destination):
        with server(app, destination) as (port, process, output):
            with contextlib.closing(http.client.HTTPConnection('127.0.0.1', port, timeout=8)) as connection:
                prefix = b'--' + BOUNDARY + b'-not-a-delimiter\r\npreamble\r\n--' + BOUNDARY + b' \t\r\n' + header(
                    '内容'.encode(), b'../a\\".bin', [b'Content-Type: application/octet-stream', b'X-Tag: first', b'x-tag: second'])
                lookalikes = b'\r\n--' + BOUNDARY + b'X\r\n--' + BOUNDARY + b'--x\r\n--' + BOUNDARY[:-1]
                lookalikes += b'\r\n--' + BOUNDARY + b' ' * 1025 + b'x\r\n--' + BOUNDARY + b'\rX'
                payload = b'F' + lookalikes + bytes(range(256)) * 8192
                suffix = b'\r\n--' + BOUNDARY + b'\r\n' + header(b'skip') + b'ignore\r\n--' + BOUNDARY + b'-- \t\r\nepilogue'
                connection.putrequest('POST', '/gated')
                connection.putheader('Content-Type', MEDIA)
                connection.putheader('Content-Length', str(len(prefix) + len(payload) + len(suffix)))
                connection.endheaders()
                connection.send(prefix)
                self.assertEqual(output.line(), b'part ready\n')
                connection.send(payload[:1])
                self.assertEqual(output.line(), b'first delivered\n')
                connection.send(payload[1:])
                connection.send(suffix)
                connection_before = connection.sock
                self.assertEqual(response(connection), (200, f'内容:{len(payload)};skipped;'.encode()))
                self.assertEqual(destination.read_bytes(), payload)
                compact = multipart((header(b'first'), b''), (header(b'second'), b'0123456789abcdef'))
                self.assertEqual(upload(connection, (bytes([byte]) for byte in compact)), (200, b'first:;second:0123456789abcdef;'))
                self.assertIs(connection.sock, connection_before)
                self.assertEqual(upload(connection, compact, '/stale'), (200, b'stale checked'))
                self.assertEqual(upload(connection, b'--' + BOUNDARY + b'--'), (200, b''))

            self.check_failures(port)
            with socket.create_connection(('127.0.0.1', port), timeout=8) as connection:
                connection.sendall(b'POST /cancel HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4096\r\nContent-Type: ' + MEDIA.encode() + b'\r\n\r\n')
                self.assertEqual(output.line(), b'waiting for cancellation\n')
                process.stdin.write(b'cancel\n')
                process.stdin.flush()
                self.assertEqual(connection.recv(1), b'')
                self.assertEqual(output.line(), b'lease released\n')
            with contextlib.closing(http.client.HTTPConnection('127.0.0.1', port, timeout=5)) as connection:
                connection.request('GET', '/ok')
                self.assertEqual(response(connection), (200, b'ok'))
            self.assertIsNone(process.poll())

    def check_failures(self, port):
        field = header(b'field')
        cases = [
            ('/upload', multipart((field, b'')), None, (415, b'media')),
            ('/upload', multipart((field, b'')), MEDIA + '; boundary=other', (415, b'media')),
            ('/upload', b'--' + BOUNDARY + b'\r\n' + field + b'abc\r\n--' + BOUNDARY + b'-', MEDIA, (400, b'truncated')),
            ('/upload', multipart((b'Content-Disposition: form-data; name="x"\n\n', b'x')), MEDIA, (400, b'malformed')),
            ('/upload', multipart((header(b'first', extra=[b'Content-Disposition: form-data; name="second"']), b'')), MEDIA, (400, b'malformed')),
            ('/upload', multipart((header(b'\xff'), b'')), MEDIA, (400, b'malformed')),
            ('/upload', multipart((header(b'first', extra=[b'X-Large: ' + b'a' * 256]), b'')), MEDIA, (413, b'headers')),
            ('/upload', multipart((field, b''), (field, b''), (field, b'')), MEDIA, (413, b'parts')),
            ('/upload', multipart((header(b'skip'), b'a' * 17)), MEDIA, (413, b'field')),
            ('/tiny', multipart((header(b'file', b'empty'), b'a' * 5)), MEDIA, (413, b'file')),
            ('/total', multipart((field, b'a')) + b'e' * 128, MEDIA, (413, b'total')),
        ]
        for path, body, media, expected in cases:
            with self.subTest(case=expected), contextlib.closing(http.client.HTTPConnection('127.0.0.1', port, timeout=5)) as connection:
                self.assertEqual(upload(connection, body, path, media), expected)


if __name__ == '__main__':
    unittest.main()
