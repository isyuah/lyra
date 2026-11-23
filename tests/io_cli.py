import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


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
                output = self.package / "app"
                built = subprocess.run([COMPILER, "build", self.package, "-o", output, *flags], capture_output=True, timeout=30)
                self.assertEqual(built.returncode, 0, built.stderr.decode())
                invoke(output)

    def test_utf8_args_file_stdin_and_stderr(self):
        source = r'''
import std.io.argCount
import std.io.arg
import std.io.readFile
import std.io.writeFile
import std.io.readLine
import std.io.writeError
import std.io.assert
fn main(): Unit {
    assert(argCount() == 2, "argc")
    assert(!arg(-1).ok() && !arg(2).ok(), "range")
    val destination = arg(0).value
    val word = arg(1).value
    assert(writeFile(destination, word + "\0尾") == 0, "write")
    val content = readFile(destination)
    assert(content.ok(), "read")
    print(content.value)
    val empty = readLine()
    assert(empty.ok() && empty.value == "", "empty line")
    print(readLine().value)
    val last = readLine()
    assert(last.ok() && last.value == "last", "no final LF")
    assert(readLine().eof(), "EOF")
    assert(writeError("诊断\n") == 0, "stderr")
}
'''
        def invoke(app):
            destination = self.package / "中文 path.txt"
            result = subprocess.run([app, destination, "参数"], input="\r\n正文\r\nlast".encode(), capture_output=True, timeout=5)
            self.assertEqual((result.returncode, result.stdout, result.stderr), (0, "参数\0尾\n正文\n".encode(), "诊断\n".encode()))
            self.assertEqual(destination.read_bytes(), "参数\0尾".encode())
            dynamic = subprocess.check_output(["readelf", "-dW", app], text=True)
            self.assertNotIn("libstdc++", dynamic)
            self.assertNotIn("libsqlite3", dynamic)
        self.example(source, invoke)

    def test_invalid_utf8_empty_file_and_missing_file(self):
        def invoke(app):
            invalid, empty, missing = (self.package / name for name in ("invalid", "empty", "missing"))
            invalid.write_bytes(b"ab\xed\xa0\x80")
            empty.write_bytes(b"")
            for raw in (b"\xc0\x80\n", b"\xf4\x90\x80\x80\n", b"\xe2\x82\n"):
                result = subprocess.run([app, invalid, empty, missing], input=raw, capture_output=True, timeout=5)
                self.assertEqual((result.returncode, result.stdout, result.stderr), (0, b"", b""))
        self.example(r'''
import std.io.arg
import std.io.readFile
import std.io.readLine
import std.io.writeFile
import std.io.assert
fn main(): Unit {
    val bad = readFile(arg(0).value)
    val empty = readFile(arg(1).value)
    val missing = readFile(arg(2).value)
    assert(bad.status > 0 && missing.status > 0 && bad.status != missing.status, "file error")
    assert(empty.ok() && empty.value == "", "empty file")
    assert(writeFile(arg(0).value + "\0ignored", "bad") > 0, "NUL path")
    val invalid = readLine()
    assert(invalid.status > 0 && !invalid.eof(), "invalid UTF-8")
    assert(readLine().eof(), "next EOF")
}
''', invoke)

    def test_foreign_scalar_and_assertion_failure(self):
        def invoke(app):
            result = subprocess.run([app], capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual((result.stdout, result.stderr), (b"12\n", "assertion failed: 预期失败\n".encode()))
        self.example('import std.io.assert\nextern fn llabs(number: Int): Int;\nfn main(): Unit { print(llabs(-12)); assert(false, "预期失败") }', invoke)

    def test_no_native_layout_for_aggregate(self):
        self.source.write_text("class Box(val number: Int)\nextern fn bad(box: Box): Int;\nfn main(): Unit {}")
        result = subprocess.run([COMPILER, "check", self.package, "--diagnostics=json"], capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        error, = json.loads(result.stderr)
        self.assertEqual(error["code"], "LYF0001")
        self.assertEqual(error["range"]["start"]["line"], 2)


if __name__ == "__main__":
    unittest.main()
