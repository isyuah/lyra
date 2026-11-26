import gzip
import io
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest
import zlib

from platform_cli import assert_linkage, executable, isolated_environment


COMPILER = Path(sys.argv.pop(1)).resolve()

SOURCE = r'''
import std.compression.{GzipDecoder, GzipEncoder, GzipError, GzipErrorKind}
import std.io.{File, IoError, readBytes, writeBytes}

fn requireIo<T>(result: Result<T, IoError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

fn requireGzip<T>(result: Result<T, GzipError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

fn failed<T>(result: Result<T, GzipError>, kind: GzipErrorKind): Bool {
    match result { Ok(_) => false, Err(error) => error.kind == kind }
}

fn encode(): Unit {
    val source = requireIo(File.openRead("原始 🙂.bin"))
    defer source.close()
    val destination = requireIo(File.openWrite("encoded.gz"))
    defer destination.close()
    val encoder = requireGzip(GzipEncoder.open(level: 6, maxInputChunkBytes: 8192))
    defer { val _ = encoder.close() }
    #assert(requireGzip(encoder.push(b"")) == b"")
    #assert(failed(encoder.push(requireIo(readBytes("large.bin"))), GzipErrorKind.InputTooLarge))
    val first = requireGzip(encoder.push(b"first\x00\xff", flush: true))
    requireIo(writeBytes("flush.gz", first))
    requireIo(destination.write(first))
    requireIo(destination.write(requireGzip(encoder.push(b"", flush: true))))
    while true {
        match requireIo(source.read(8192)) {
            None => break,
            Some(content) => requireIo(destination.write(requireGzip(encoder.push(content)))),
        }
    }
    requireIo(destination.write(requireGzip(encoder.finish())))
    #assert(failed(encoder.finish(), GzipErrorKind.Closed))
    requireGzip(encoder.close())
}

fn decode(path: String, destination: File, inputChunkBytes: Int, budget: Int): Unit {
    val source = requireIo(File.openRead(path))
    defer source.close()
    val decoder = requireGzip(GzipDecoder.open(budget, inputChunkBytes, outputChunkBytes: 127))
    defer { val _ = decoder.close() }
    #assert(requireGzip(decoder.read()) == None, "no input yet")
    while true {
        match requireIo(source.read(inputChunkBytes)) {
            None => break,
            Some(content) => requireGzip(decoder.push(content)),
        }
        #assert(failed(decoder.push(b"ignore"), GzipErrorKind.Busy))
        #assert(failed(decoder.finish(), GzipErrorKind.Busy))
        if inputChunkBytes > 1 { gcCollect() }
        while true {
            match requireGzip(decoder.read()) {
                Some(content) => {
                    #assert(content.size > 0 && content.size <= 127)
                    requireIo(destination.write(content))
                },
                None => break,
            }
        }
    }
    requireGzip(decoder.finish())
    #assert(failed(decoder.read(), GzipErrorKind.Closed))
}

fn reject(path: String, expected: GzipErrorKind, budget: Int = 8388608): Unit {
    val decoder = requireGzip(GzipDecoder.open(budget, outputChunkBytes: 101))
    defer { val _ = decoder.close() }
    requireGzip(decoder.push(requireIo(readBytes(path))))
    while true {
        match decoder.read() {
            Err(error) => {
                #assert(error.kind == expected, error.message())
                #assert(failed(decoder.read(), GzipErrorKind.Closed))
                return
            },
            Ok(None) => break,
            Ok(Some(_)) => {},
        }
    }
    #assert(failed(decoder.finish(), expected), path)
    #assert(failed(decoder.read(), GzipErrorKind.Closed))
}

fn main(): Unit {
    #assert(failed(GzipEncoder.open(level: 10), GzipErrorKind.Invalid))
    #assert(failed(GzipEncoder.open(maxInputChunkBytes: 0), GzipErrorKind.Invalid))
    #assert(failed(GzipEncoder.open(maxOutputChunkBytes: 16777217), GzipErrorKind.Invalid))
    #assert(failed(GzipDecoder.open(maxOutputBytes: -1), GzipErrorKind.Invalid))
    #assert(failed(GzipDecoder.open(outputChunkBytes: 0), GzipErrorKind.Invalid))
    val limited = requireGzip(GzipEncoder.open(maxOutputChunkBytes: 1))
    #assert(failed(limited.finish(), GzipErrorKind.OutputTooLarge))
    #assert(failed(limited.push(b"a"), GzipErrorKind.Closed))
    requireGzip(limited.close())
    requireGzip(limited.close())

    val rangeEncoder = requireGzip(GzipEncoder.open(maxInputChunkBytes: 3))
    #assert(failed(rangeEncoder.pushRange(b"small", -1, 2), GzipErrorKind.Invalid))
    #assert(failed(rangeEncoder.pushRange(b"small", 2, 1), GzipErrorKind.Invalid))
    #assert(failed(rangeEncoder.pushRange(b"small", 1, 6), GzipErrorKind.Invalid))
    val ranges = requireIo(File.openWrite("range.gz"))
    requireIo(ranges.write(requireGzip(rangeEncoder.pushRange(b"before\0\xffZafter", 6, 9, flush: true))))
    requireIo(ranges.write(requireGzip(rangeEncoder.finish())))
    ranges.close()

    val emptyEncoder = requireGzip(GzipEncoder.open())
    val emptyDecoder = requireGzip(GzipDecoder.open(maxOutputBytes: 0, outputChunkBytes: 1))
    requireGzip(emptyDecoder.push(requireGzip(emptyEncoder.finish())))
    #assert(requireGzip(emptyDecoder.read()) == None)
    requireGzip(emptyDecoder.finish())

    val decoder = requireGzip(GzipDecoder.open(maxInputChunkBytes: 2))
    #assert(failed(decoder.push(b"123"), GzipErrorKind.InputTooLarge))
    requireGzip(decoder.push(b""))
    #assert(requireGzip(decoder.read()) == None)
    #assert(failed(decoder.finish(), GzipErrorKind.Truncated))
    requireGzip(decoder.close())

    encode()
    val expanded = requireIo(File.openWrite("expanded.bin"))
    decode("members.gz", expanded, 1, 70020)
    expanded.close()
    val roundTrip = requireIo(File.openWrite("roundtrip.bin"))
    decode("encoded.gz", roundTrip, 65536, 180007)
    roundTrip.close()

    reject("crc.gz", GzipErrorKind.InvalidData)
    reject("size.gz", GzipErrorKind.InvalidData)
    reject("garbage.gz", GzipErrorKind.InvalidData)
    reject("zlib.bin", GzipErrorKind.InvalidData)
    reject("truncated.gz", GzipErrorKind.Truncated)
    reject("truncated-member.gz", GzipErrorKind.Truncated)
    reject("members.gz", GzipErrorKind.OutputTooLarge, budget: 70019)
    reject("members.gz", GzipErrorKind.OutputTooLarge, budget: 0)
}
'''


class Compression(unittest.TestCase):
    def test_interoperable_bounded_streams(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'src').mkdir()
            (root / 'src/main.ly').write_text(SOURCE, encoding='utf-8')
            (root / 'lyra.json5').write_text('{name:"test/compression",version:"0.1.0"}', encoding='utf-8')
            random_payload = random.Random(17).randbytes(180000)
            (root / '原始 🙂.bin').write_bytes(random_payload)
            (root / 'large.bin').write_bytes(random_payload[:8193])

            binary = b'\x00\xff' * 10
            first = io.BytesIO()
            with gzip.GzipFile(filename='payload', mode='wb', fileobj=first, mtime=0) as member:
                member.write(binary)
            compressed = gzip.compress(b'x' * 70000, mtime=0)
            members = first.getvalue() + gzip.compress(b'', mtime=0) + compressed
            fixtures = {
                'members.gz': members,
                'crc.gz': compressed[:-8] + bytes([compressed[-8] ^ 1]) + compressed[-7:],
                'size.gz': compressed[:-4] + bytes([compressed[-4] ^ 1]) + compressed[-3:],
                'garbage.gz': members + b'junk',
                'truncated.gz': compressed[:-1],
                'truncated-member.gz': members + b'\x1f\x8b',
                'zlib.bin': zlib.compress(binary),
            }
            for name, content in fixtures.items():
                (root / name).write_bytes(content)

            for flags in ([], ['--release']):
                with self.subTest(flags=flags):
                    app = executable(root / ('release' if flags else 'debug'))
                    built = subprocess.run([COMPILER, 'build', root, '-o', app, *flags], capture_output=True, timeout=45)
                    self.assertEqual(built.returncode, 0, built.stderr.decode())
                    assert_linkage(self, app)
                    run = subprocess.run([app], capture_output=True, cwd=root, env=isolated_environment(os.environ), timeout=15)
                    self.assertEqual((run.returncode, run.stdout, run.stderr), (0, b'', b''))
                    partial = zlib.decompressobj(wbits=31)
                    self.assertEqual(partial.decompress((root / 'flush.gz').read_bytes()), b'first\0\xff')
                    self.assertFalse(partial.eof)
                    self.assertEqual(gzip.decompress((root / 'range.gz').read_bytes()), b'\0\xffZ')
                    self.assertEqual(gzip.decompress((root / 'encoded.gz').read_bytes()), b'first\0\xff' + random_payload)
                    self.assertEqual((root / 'expanded.bin').read_bytes(), binary + b'x' * 70000)
                    self.assertEqual((root / 'roundtrip.bin').read_bytes(), b'first\0\xff' + random_payload)


if __name__ == '__main__':
    unittest.main()
