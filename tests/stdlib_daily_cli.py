import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from platform_cli import c_archive, executable, running


COMPILER = Path(sys.argv.pop(1)).resolve()


class DailyStdlib(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.package = Path(temporary.name)
        (self.package / 'src').mkdir()
        (self.package / 'lyra.json5').write_text('{name:"test/daily",version:"0.1.0"}')

    def build(self, source, *flags):
        (self.package / 'src/main.ly').write_text(source, encoding='utf-8')
        output = executable(self.package / 'app')
        result = subprocess.run([COMPILER, 'build', self.package, '-o', output, *flags], capture_output=True, timeout=40)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        return output

    def run_native(self, app, *args, expected=b''):
        result = subprocess.run([app, *args], capture_output=True, timeout=8)
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, expected, b''))

    def test_list_methods_are_ordered_short_circuiting_and_preserve_nested_none(self):
        source = '''
import std.collections.ListOps
import std.collections.any as anyValue
import std.collections.find as findValue
import std.io.assert

fn main(): Unit {
    var visits = 0
    val matched = [1, 2, 3, 4].filter { it >= 2 }.map { it * 2 }.find { visits += 1; it == 6 }
    assert(visits == 2 && (matched ?? 0) == 6, "find stops immediately")
    val found = anyValue([1, 2, 3]) { visits += 1; it > 1 }
    assert(found && visits == 4, "any stops immediately")
    val optional: List<Int?> = [None, Some(1)]
    val presentNull = findValue(optional) { it == None }
    assert(match presentNull { Some(None) => true, _ => false }, "found null")
    val empty: List<Int> = []
    assert(!empty.any { visits += 1; true } && visits == 4, "empty any")
    assert((empty.find { visits += 1; true }) == None && visits == 4, "empty find")
    assert([2, 3].map { it * 2 }.fold(1, (sum, value) => { sum + value }) == 11, "chained fold")
}
'''
        for flags in ((), ('--release',)):
            with self.subTest(flags=flags):
                self.run_native(self.build(source, *flags))

    def test_binary_files_preserve_data_and_separate_eof_from_io_errors(self):
        source = r'''
import std.io.File
import std.io.IoError
import std.io.IoErrorKind
import std.io.arg
import std.io.assert
import std.io.readBytes
import std.io.readFile
import std.io.writeBytes

fn copyFile(source: String, destination: String): Result<Int, IoError> {
    val input = File.openRead(source)?
    defer input.close()
    val output = File.openWrite(destination)?
    defer output.close()
    assert(match input.read(0) { Err(error) => match error.kind { Invalid => true, _ => false }, _ => false }, "non-positive size is an error")
    assert(match output.read(1) { Err(error) => match error.kind { Invalid => true, _ => false }, _ => false }, "write-only handle is not EOF")
    var copied = 0
    while true {
        match input.read(70001)? {
            None => break,
            Some(chunk) => {
                assert(chunk.size > 0 && chunk.size <= 65536, "bounded non-empty chunks")
                copied += chunk.size
                output.write(chunk)?;
            },
        }
    }
    output.flush()?
    val alias = input
    input.close()
    assert(match alias.read() { Err(error) => match error.kind { Closed => true, _ => false }, _ => false }, "closed handle is not EOF")
    alias.close()
    Ok(copied)
}

fn main(): Unit {
    val source = arg(0).value
    val destination = arg(1).value
    val empty = arg(2).value
    val invalidText = readFile(source)
    assert(!invalidText.ok() && invalidText.invalidUtf8(), "text path still checks UTF-8")
    val saved = match readBytes(source) { Ok(data) => data, Err(error) => throw Exception(error.message()) }
    assert(saved.size > 65536 && saved[0] == 0 && saved[255] == 255, "reads arbitrary bytes")
    assert(match writeBytes(empty, b"") { Ok(_) => true, Err(_) => false }, "creates empty file")
    assert(match readBytes(empty) { Ok(data) => data == b"", _ => false }, "empty read succeeds")
    assert(match readBytes(empty + ".missing") { Err(error) => (error.code() ?? 0) > 0, _ => false }, "missing input is a system error")
    assert(match writeBytes(empty + "\0extra", b"!") { Err(error) => error.operation == "writeBytes", _ => false }, "NUL in a path is rejected")
    val size = match copyFile(source, destination) { Ok(length) => length, Err(error) => throw Exception(error.message()) }
    assert(size == saved.size, "copy bytes")
    gcCollect()
    assert(match readBytes(destination) { Ok(data) => data == saved, _ => false }, "data survives close and GC")
}
'''
        original = self.package / '二进制 源'
        destination = self.package / '二进制 目标'
        empty = self.package / 'empty'
        payload = bytes(range(256)) * 302
        original.write_bytes(payload)
        for flags in ((), ('--release',)):
            with self.subTest(flags=flags):
                destination.write_bytes(b'not the new content' * 5000)
                empty.write_bytes(b'already has content')
                self.run_native(self.build(source, *flags), original, destination, empty)
                self.assertEqual(destination.read_bytes(), payload)
                self.assertEqual(empty.read_bytes(), b'')

    @unittest.skipUnless(hasattr(os, 'mkfifo'), 'requires a POSIX FIFO')
    def test_close_stays_busy_while_a_read_owns_the_handle(self):
        app = self.build('''
import std.io.Busy
import std.io.File
import std.io.IoErrorKind
import std.io.arg
import std.io.assert

async fn checkpoint(): Unit {}

async fn main(): Unit {
    val file = match File.openRead(arg(0).value) { Ok(handle) => handle, Err(error) => throw Exception(error.message()) }
    val alias = file
    val operation = spawn blocking { file.read(1) }
    var busy = false
    while !busy {
        await checkpoint()
        busy = match alias.flush() { Err(error) => match error.kind { Busy => true, _ => false }, _ => false }
    }
    var refused = false
    try { alias.close() } catch error: Busy { refused = true }
    assert(refused, "close must let the live read finish")
    print("leased")
    assert(match await operation { Ok(Some(data)) => data == b"!", _ => false }, "lease survives Busy close")
    file.close()
    alias.close()
    print("closed")
}
''')
        fifo = self.package / 'read gate'
        os.mkfifo(fifo)
        gate = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        try:
            with running([app, fifo]) as (process, lines):
                self.assertEqual(lines.line(), b'leased\n')
                os.write(gate, b'!')
                self.assertEqual(lines.remaining(), b'closed\n')
                self.assertEqual(process.wait(timeout=5), 0)
        finally:
            os.close(gate)

    @unittest.skipUnless(Path('/dev/full').exists(), 'requires a device that refuses writes')
    def test_write_bytes_reports_buffered_write_failure_and_preserves_error(self):
        app = self.build('''
import std.io.IoError
import std.io.arg
import std.io.assert
import std.io.writeBytes

fn write(path: String): Result<Unit, Error> { writeBytes(path, b"data")?; Ok({}) }
fn main(): Unit {
    assert(match write(arg(0).value) {
        Err(error) => error.original is IoError && error.message != "",
        _ => false,
    }, "flush failure must propagate")
}
''')
        self.run_native(app, '/dev/full')

    def test_bytes_foreign_view_has_unsigned_length_bound_content(self):
        native = self.package / 'view.c'
        native.write_text('''#include <lyra/string.h>
#include <stdint.h>
uint8_t inspectBytes(const LyraBytes *value) {
    LyraBytesView view = lyraBytesView(value);
    return view.size == 3 && view.data[0] == 0 && view.data[1] == 255 && view.data[2] == 13;
}
const LyraBytes *echoBytes(const LyraBytes *value) { return value; }
''')
        include = Path(__file__).resolve().parent.parent / 'include'
        archive = c_archive(native, self.package / 'view', '-I', include)
        app = self.build(r'''
import std.io.assert
extern fn inspectBytes(value: Bytes): Bool;
extern fn echoBytes(value: Bytes): Bytes;
fn main(): Unit {
    val expected = b"\0\xff\r"
    assert(inspectBytes(expected) && echoBytes(expected) == expected, "C Bytes view")
}
''', '--release', '--link', archive)
        self.run_native(app)


if __name__ == '__main__':
    unittest.main()
