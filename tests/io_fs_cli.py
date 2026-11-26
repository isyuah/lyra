import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from platform_cli import executable


COMPILER = Path(sys.argv.pop(1)).resolve()

REQUIRE = '''
import std.io.{IoError}
import std.io.{IoErrorKind}

fn require<T>(result: Result<T, IoError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

fn hasError<T>(result: Result<T, IoError>, kind: IoErrorKind): Bool {
    match result { Err(error) => error.kind == kind, Ok(_) => false }
}
'''


class IoFs(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.package = Path(temporary.name)
        (self.package / 'src').mkdir()
        (self.package / 'lyra.json5').write_text('{name:"test/io-fs",version:"0.1.0"}', encoding='utf-8')

    def build(self, source):
        (self.package / 'src/main.ly').write_text(REQUIRE + source, encoding='utf-8')
        app = executable(self.package / 'app')
        result = subprocess.run([COMPILER, 'build', self.package, '-o', app], capture_output=True, timeout=45)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        return app

    def run_app(self, source, data=b'', expected_error=b''):
        result = subprocess.run([self.build(source)], input=data, capture_output=True, cwd=self.package, timeout=8)
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, b'', expected_error))

    def test_stdin_and_files_consume_bad_lines_and_share_their_cursor(self):
        data = '🙂\r\n'.encode() + b'12345\r\n' + b'x' * 130000 + b'\n\xff\n\nz\0\r\nlit\r'
        (self.package / '有界 输入.txt').write_bytes(data)
        self.run_app(r'''
import std.io.{File}
import std.io.{LineReader}
import std.io.{readLine}

fn remainingLines(reader: LineReader): Unit {
    #assert(hasError(reader.readLine(), IoErrorKind.LineTooLong), "one byte over limit")
    #assert(hasError(reader.readLine(), IoErrorKind.LineTooLong), "drain long line without splitting it")
    #assert(hasError(reader.readLine(), IoErrorKind.InvalidUtf8), "invalid UTF-8 is not EOF")
    #assert(require(reader.readLine()) == Some(""), "blank line")
    #assert(require(reader.readLine()) == Some("z\0"), "embedded NUL")
    #assert(require(reader.readLine()) == Some("lit\r"), "last line keeps its final CR without LF")
    #assert(require(reader.readLine()) == None && require(reader.readLine()) == None, "EOF is stable")
}

fn main(): Unit {
    #assert(hasError(readLine(0), IoErrorKind.Invalid), "invalid limit does not consume input")
    #assert(require(readLine(maxBytes: 4)) == Some("🙂"), "byte limit excludes CRLF")
    remainingLines(require(LineReader.stdin(4)))
    val file = require(File.openRead("有界 输入.txt"))
    defer file.close()
    #assert(hasError(LineReader.fromFile(file, -1), IoErrorKind.Invalid), "reject negative size")
    val first = require(LineReader.fromFile(file, 4))
    #assert(require(first.readLine()) == Some("🙂"), "decode UTF-8 from file")
    remainingLines(require(LineReader.fromFile(file, 4)))
    #assert(require(file.seek(0)) == 0, "seek clears file EOF")
    #assert(require(file.read(6)) == Some(b"🙂\r\n"), "bytes share the line cursor")
    #assert(hasError(first.readLine(), IoErrorKind.LineTooLong), "reader uses the repositioned handle")
    file.close()
    #assert(hasError(first.readLine(), IoErrorKind.Closed), "reader observes handle closure")
}
''', data)

    def test_append_and_64_bit_seek_preserve_bytes_and_report_errors(self):
        self.run_app(r'''
import std.io.{File}
import std.io.{SeekOrigin}
import std.io.{readBytes}
import std.io.{readFile}
import std.io.{writeError}
import std.io.{writeFile}

fn main(): Unit {
    val path = "追加 文件🙂"
    require(writeFile(path, "首\0"))
    #assert(require(readFile(path)) == "首\0", "text round trip")
    #assert(hasError(readFile(path + "\0x"), IoErrorKind.Invalid), "NUL path is invalid")
    val append = require(File.openAppend(path))
    defer append.close()
    #assert(require(append.seek(0)) == 0, "seek on append handle")
    require(append.write(b"\xffx"))
    require(append.flush())
    append.close()
    #assert(require(readBytes(path)) == b"首\0\xffx", "append does not overwrite")
    #assert(hasError(readFile(path), IoErrorKind.InvalidUtf8), "typed text failure")
    val input = require(File.openRead(path))
    defer input.close()
    #assert(require(input.seek(4294967301)) == 4294967301, "64-bit file offset without writing a sparse file")
    #assert(require(input.seek(-1, origin: SeekOrigin.Current)) == 4294967300, "relative offset")
    #assert(require(input.seek(-2, origin: SeekOrigin.End)) == 4, "offset from end")
    #assert(require(input.read()) == Some(b"\xffx"), "read from target offset")
    #assert(require(input.read()) == None, "byte EOF")
    input.close()
    #assert(hasError(input.seek(0), IoErrorKind.Closed), "closed seek")
    val newFile = require(File.openAppend("new.txt"))
    newFile.close()
    #assert(require(readFile("new.txt")) == "", "append creates a missing file")
    require(writeError("stderr\0"))
}
''', expected_error=b'stderr\0')

    def test_directory_snapshot_temp_file_and_atomic_replace(self):
        if os.name == 'nt':
            path_cases = r'''
    #assert(pathJoin(r"C:\base", r"\kid") == r"C:\kid", "rooted path keeps drive")
    #assert(pathJoin(r"C:base", r"d:other") == r"d:other", "different drive replaces base")
    #assert(pathJoin(r"C:base", r"c:kid") == r"C:base\kid", "same drive relative path")
    #assert(pathJoin(r"\\host\share\base", r"\kid") == r"\\host\share\kid", "UNC root")
'''
        else:
            path_cases = r'''
    #assert(pathJoin("left", "/absolute", "next") == "/absolute/next", "absolute replaces base")
    #assert(pathJoin("left", r"a\b") == r"left/a\b", "POSIX backslash is not a separator")
'''
        self.run_app(r'''
import std.fs.{atomicWrite}
import std.fs.{createTempFile}
import std.fs.{listDir}
import std.fs.{mkdirAll}
import std.fs.{pathJoin}
import std.fs.{removeFile}
import std.io.{readBytes}

fn contains(values: List<String>, name: String): Bool {
    for value in values { if value == name { return true } }
    false
}

fn main(): Unit {
''' + path_cases + r'''
    #assert(pathJoin("", "", "one", "") == "one", "empty segments")
    val directory = pathJoin("发布 文件", "测试🙂")
    require(mkdirAll(directory))
    require(mkdirAll(directory))
    val target = pathJoin(directory, "target.bin")
    require(atomicWrite(target, b"old"))
    require(atomicWrite(target, b"\0new\xff"))
    #assert(require(readBytes(target)) == b"\0new\xff", "replace existing file completely")
    #assert(match mkdirAll(target) { Err(error) => error.code() != None, _ => false }, "file is not a directory")
    val first = require(createTempFile(directory, prefix: "临时 "))
    val second = require(createTempFile(directory, prefix: "临时 "))
    #assert(first.path != second.path, "exclusive creation")
    require(first.file.write(b"first"))
    require(second.file.write(b"second"))
    first.file.close()
    second.file.close()
    #assert(require(readBytes(first.path)) == b"first" && require(readBytes(second.path)) == b"second", "temp handles match paths")
    #assert(hasError(createTempFile(directory, prefix: "../bad"), IoErrorKind.Invalid), "prefix not a subpath")
    require(removeFile(first.path))
    require(removeFile(second.path))
    val occupied = pathJoin(directory, "subdir")
    require(mkdirAll(occupied))
    #assert(match atomicWrite(occupied, b"keep directory") { Err(error) => error.code() != None, _ => false }, "failed replacement is reported")
    val names = require(listDir(directory))
    #assert(names.size() == 2 && contains(names, "target.bin") && contains(names, "subdir"), "no leftover temp and names are relative")
    #assert(hasError(listDir(directory + "\0other"), IoErrorKind.Invalid), "NUL path is invalid")
}
''')

    @unittest.skipIf(os.name == 'nt', 'Windows filenames are Unicode')
    def test_non_utf8_entry_is_an_error_not_a_truncated_snapshot(self):
        directory = self.package / 'invalid'
        directory.mkdir()
        with open(os.fsencode(directory) + b'/\xff', 'wb'):
            pass
        self.run_app('''
import std.fs.{listDir}

fn main(): Unit {
    #assert(hasError(listDir("invalid"), IoErrorKind.InvalidUtf8), "do not drop non-UTF-8 names")
}
''')

    def test_stat_and_explicit_overwrite_keep_file_contents(self):
        self.run_app(r'''
import std.fs.{FileKind, copy, cwd, exists, isDir, mkdirAll, pathJoin, removeDir, removeFile, rename, stat}
import std.io.{readBytes, writeBytes}

fn main(): Unit {
    val folder = "复制 目录🙂"
    require(mkdirAll(folder))
    val source = pathJoin(folder, "来源.txt")
    val target = pathJoin(folder, "目标.txt")
    #assert(!require(exists(source)) && !require(isDir(source)))
    #assert(require(stat(source)) == None)
    #assert(require(isDir(pathJoin(require(cwd()), folder))))
    require(writeBytes(source, b"content\0"))
    require(writeBytes(target, b"original"))
    #assert(match require(stat(source)) { Some(info) => info.kind == FileKind.Regular && info.size == 8, None => false })
    #assert(match copy(source, target) { Err(_) => true, Ok(_) => false })
    #assert(match rename(source, target) { Err(_) => true, Ok(_) => false })
    #assert(require(readBytes(source)) == b"content\0" && require(readBytes(target)) == b"original")
    require(copy(source, target, replace: true))
    #assert(require(readBytes(target)) == b"content\0")
    require(writeBytes(source, b"moved"))
    require(rename(source, target, replace: true))
    #assert(!require(exists(source)) && require(readBytes(target)) == b"moved")
    require(rename(target, source))
    #assert(require(exists(source)) && !require(exists(target)))
    #assert(match removeDir(folder) { Err(_) => true, Ok(_) => false })
    #assert(hasError(exists("\0"), IoErrorKind.Invalid))
    require(removeFile(source))
    require(removeDir(folder))
    #assert(!require(exists(folder)))
}
''')

    @unittest.skipIf(os.name == 'nt', 'creating symlinks may require Windows developer mode')
    def test_stat_can_inspect_dangling_symlinks(self):
        (self.package / 'dangling').symlink_to('absent')
        self.run_app('''
import std.fs.{FileKind, exists, stat}

fn main(): Unit {
    #assert(!require(exists("dangling")))
    #assert(require(exists("dangling", followSymlinks: false)))
    #assert(match require(stat("dangling", followSymlinks: false)) { Some(info) => info.kind == FileKind.Symlink, None => false })
}
''')


if __name__ == '__main__':
    unittest.main()
