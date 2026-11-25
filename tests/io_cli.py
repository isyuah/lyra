import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from platform_cli import assert_linkage, c_archive, executable


COMPILER = Path(sys.argv.pop(1)).resolve()

IO_HELPERS = '''
import std.io.{IoError}
import std.io.{IoErrorKind}
import std.os.{args}

fn require<T>(result: Result<T, IoError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

fn failed<T>(result: Result<T, IoError>, kind: IoErrorKind): Bool {
    match result { Err(error) => error.kind == kind, _ => false }
}
'''


class IO(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.package = Path(temporary.name)
        (self.package / "src").mkdir()
        (self.package / "lyra.json5").write_text("{name:'test/io',version:'1.0.0'}")
        self.source = self.package / "src/main.ly"

    def example(self, source, invoke):
        self.source.write_text(source, encoding="utf-8")
        for flags in ([], ["--release"]):
            with self.subTest(release=bool(flags)):
                output = executable(self.package / "app")
                built = subprocess.run([COMPILER, "build", self.package, "-o", output, *flags], capture_output=True, timeout=30)
                self.assertEqual(built.returncode, 0, built.stderr.decode())
                invoke(output)

    def test_utf8_args_file_stdin_and_stderr(self):
        source = IO_HELPERS + r'''
import std.io.{readFile}
import std.io.{writeFile}
import std.io.{readLine}
import std.io.{writeError}
fn main(): Unit {
    val arguments = require(args())
    #assert(arguments.size == 2, "argc")
    val destination = arguments[0]
    val word = arguments[1]
    require(writeFile(destination, word + "\0尾"))
    print(require(readFile(destination)))
    #assert(require(readLine()) == Some(""), "empty line")
    print(require(readLine()) ?? "missing")
    #assert(require(readLine()) == Some("last"), "no final LF")
    #assert(require(readLine()) == None, "EOF")
    require(writeError("诊断\n"))
}
'''
        def invoke(app):
            destination = self.package / "中文 path.txt"
            result = subprocess.run([app, destination, "参数"], input="\r\n正文\r\nlast".encode(), capture_output=True, timeout=5)
            self.assertEqual((result.returncode, result.stdout, result.stderr), (0, "参数\0尾\n正文\n".encode(), "诊断\n".encode()))
            self.assertEqual(destination.read_bytes(), "参数\0尾".encode())
            assert_linkage(self, app)
        self.example(source, invoke)

    def test_invalid_utf8_empty_file_and_missing_file(self):
        def invoke(app):
            invalid, empty, missing = (self.package / name for name in ("invalid", "empty", "missing"))
            invalid.write_bytes(b"ab\xed\xa0\x80")
            empty.write_bytes(b"")
            for raw in (b"\xc0\x80\n", b"\xf4\x90\x80\x80\n", b"\xe2\x82\n"):
                result = subprocess.run([app, invalid, empty, missing], input=raw, capture_output=True, timeout=5)
                self.assertEqual((result.returncode, result.stdout, result.stderr), (0, b"", b""))
        self.example(IO_HELPERS + r'''
import std.io.{readFile}
import std.io.{readLine}
import std.io.{writeFile}
fn main(): Unit {
    val arguments = require(args())
    #assert(failed(readFile(arguments[0]), IoErrorKind.InvalidUtf8), "invalid file")
    #assert(require(readFile(arguments[1])) == "", "empty file")
    #assert(match readFile(arguments[2]) { Err(error) => error.code() != None, _ => false }, "missing file")
    #assert(failed(writeFile(arguments[0] + "\0ignored", "bad"), IoErrorKind.Invalid), "NUL path")
    #assert(failed(readLine(), IoErrorKind.InvalidUtf8), "invalid UTF-8")
    #assert(require(readLine()) == None, "next EOF")
}
''', invoke)

    def test_run_preserves_forwarded_argument_boundaries(self):
        self.source.write_text(IO_HELPERS + 'fn main(): Unit { val values = require(args()); print(values.size); print(values[0]); print(values[1]) }')
        value = '中文 "quoted" \\'
        result = subprocess.run([COMPILER, 'run', self.package, '--', '', value], capture_output=True, timeout=30)
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, ('2\n\n' + value + '\n').encode(), b''))

    def test_foreign_scalar_and_assertion_failure(self):
        def invoke(app):
            result = subprocess.run([app], capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b"12\n2\n1\ncleanup\n")
            self.assertIn('main.ly:7:', result.stderr.decode())
            self.assertIn('assertion failed: 预期失败', result.stderr.decode())
        self.example('''extern fn llabs(number: Int): Int;
fn main(): Unit {
    defer print("cleanup")
    print(llabs(-12))
    var calls = 0
    #assert({ calls += 1; true }, { throw Exception("must not run") })
    #assert({ calls += 1; print(calls); false }, { print(1); "预期失败" })
}
''', invoke)

    def test_no_native_layout_for_aggregate(self):
        self.source.write_text("class Box(val number: Int)\nextern fn bad(box: Box): Int;\nfn main(): Unit {}")
        result = subprocess.run([COMPILER, "check", self.package, "--diagnostics=json"], capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        error, = json.loads(result.stderr)
        self.assertEqual(error["code"], "LYF0001")
        self.assertEqual(error["range"]["start"]["line"], 2)

    def test_link_a_user_selected_c_archive(self):
        native = self.package / 'native helper.c'
        native.write_text('#include <stdint.h>\nint64_t fixtureScale(int64_t value) { return value * 3; }\n')
        archive = c_archive(native, self.package / '原生 helper')
        self.source.write_text('extern fn fixtureScale(value: Int): Int;\nfn main(): Unit { print(fixtureScale(7)) }')
        run = subprocess.run([COMPILER, 'run', self.package, '--link', archive], capture_output=True, timeout=30)
        self.assertEqual((run.returncode, run.stdout, run.stderr), (0, b'21\n', b''))
        app = executable(self.package / 'build/debug/app')
        previously = app.read_bytes()
        missing = subprocess.run([COMPILER, 'build', self.package], capture_output=True, timeout=30)
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn(b'fixtureScale', missing.stderr)
        self.assertEqual(app.read_bytes(), previously)


if __name__ == "__main__":
    unittest.main()
