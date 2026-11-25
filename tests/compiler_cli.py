import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from platform_cli import WINDOWS, assert_linkage, c_archive, executable, running


COMPILER = Path(sys.argv.pop(1)).resolve()
FIXTURES = Path(__file__).resolve().parents[1] / "examples" / "compiler-smoke"


class PackageTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.package = Path(temporary.name) / "Lyra 测试 package"
        self.src = self.package / "src"
        self.src.mkdir(parents=True)
        self.main = self.src / "main.ly"

    def cli(self, command, *options, env=None):
        return subprocess.run([COMPILER, command, self.package, *options],
                              capture_output=True, text=True, env=env, timeout=15)

    def fixture(self, kind):
        shutil.copyfile(FIXTURES / kind / "src" / "main.ly", self.main)

    def semicolon_error(self, result):
        self.assertNotEqual(result.returncode, 0)
        diagnostic, = json.loads(result.stderr)
        self.assertEqual(diagnostic["code"], "LYT0001")
        self.assertEqual((diagnostic["expected"], diagnostic["actual"]), ("Int", "Unit"))
        self.assertEqual(Path(diagnostic["file"]).name, "main.ly")
        data = self.main.read_bytes()
        start = diagnostic["range"]["start"]
        end = diagnostic["range"]["end"]
        self.assertEqual((start["offset"], end["offset"]), (data.index(b";"), data.index(b";") + 1))
        prefix = data[:start["offset"]].decode()
        self.assertEqual((start["line"], start["column"]),
                         (prefix.count("\n") + 1, len(prefix.rsplit("\n", 1)[-1]) + 1))
        self.assertTrue(diagnostic["hint"])
        return diagnostic


class Frontend(PackageTest):
    def test_check_recovers_across_files_without_cascading_from_a_failed_inference(self):
        self.main.write_text('''fn broken() { missingValue }
fn dependent(): Int { broken() }
fn main(): Unit {}
''', encoding='utf-8')
        other = self.src / 'other.ly'
        other.write_text('''fn syntax(): Int { val incomplete = ; 0 }
fn callSyntax(): Int { syntax() }
fn independent(): Int { false }
''', encoding='utf-8')
        result = self.cli('check', '--diagnostics=json')
        self.assertEqual(result.returncode, 1)
        self.assertEqual([(Path(error['file']).name, error['code'], error['range']['start']['line'])
                          for error in json.loads(result.stderr)],
                         [('main.ly', 'LYN0001', 1), ('other.ly', 'LYP0001', 1), ('other.ly', 'LYT0001', 3)])
        built = self.cli('build', '--diagnostics=json')
        self.assertEqual(built.returncode, 1)
        self.assertFalse((self.package / 'build').exists())

    def test_check_lists_overload_declarations_as_notes(self):
        self.main.write_text('''fn choose(value: Int8): Int { 1 }
fn choose(value: UInt8): Int { 2 }
fn main(): Unit { print(choose(1)) }
''', encoding='utf-8')
        result = self.cli('check', '--diagnostics=json')
        self.assertEqual(result.returncode, 1)
        error, = json.loads(result.stderr)
        self.assertEqual(error['code'], 'LYT0019')
        self.assertEqual({(Path(note['file']), note['range']['start']['line']) for note in error['notes']},
                         {(self.main.resolve(), 1), (self.main.resolve(), 2)})

    def test_check_retains_signature_after_missing_closing_brace(self):
        self.main.write_text('''fn type(): Unit {}
fn before(): Unit {
    type()
}
fn broken(): Int {
    1
fn independent(): Int { false }
fn main(): Unit { print(broken()) }
''', encoding='utf-8')
        result = self.cli('check', '--diagnostics=json')
        self.assertEqual(result.returncode, 1)
        self.assertEqual([error['code'] for error in json.loads(result.stderr)], ['LYP0001', 'LYT0001'])

    def test_check_recovers_qualified_type_errors_in_function_bodies(self):
        (self.package / 'lyra.json5').write_text('{name:"app",version:"0.1.0"}', encoding='utf-8')
        self.main.write_text('''import std.time
fn broken(): Unit { val value: time.Bad = "" }
fn independent(): Int { false }
fn main(): Unit { broken() }
''', encoding='utf-8')
        result = self.cli('check', '--diagnostics=json')
        self.assertEqual(result.returncode, 1)
        self.assertEqual([error['code'] for error in json.loads(result.stderr)], ['LYN0003', 'LYT0001'])

    def test_numeric_literals_and_conversions_reject_lossy_contexts(self):
        for declaration, code, fragment in (
            ('fn value(): Int8 { 128 }', 'LYL0002', 'Int8'),
            ('fn value(): UInt64 { -1 }', 'LYL0002', 'UInt'),
            ('fn value(): UInt { 18446744073709551616 }', 'LYL0002', '64'),
            ('fn value(): Float32 { 3.5e38 }', 'LYL0002', 'Float32'),
            ('fn value(): Float64 { 1.0e+ }', 'LYL0002', '字面量'),
            ('fn value(): Float64 { 1.5_f32 }', 'LYL0002', '_'),
            ('fn value(item: Int): Float { item }', 'LYT0001', 'Float64'),
            ('fn value(item: Float): Float32 { item }', 'LYT0001', 'Float32'),
            ('fn value(item: Int16): Int8 { item }', 'LYT0001', 'Int8'),
            ('fn value(item: Int, real: Float): Float { real + item }', 'LYT0001', '无损'),
            ('fn value(item: UInt8): UInt8 { -item }', 'LYT0006', '无符号'),
            ('fn choose<T>(a: T, b: T): T { a }\nfn value(a: Int, b: UInt): Int { choose(a, b) }', 'LYT0001', '类型参数'),
        ):
            with self.subTest(declaration=declaration):
                self.main.write_text(declaration + '\nfn main(): Unit {}', encoding='utf-8')
                result = self.cli('check', '--diagnostics=json')
                self.assertNotEqual(result.returncode, 0)
                error, = json.loads(result.stderr)
                self.assertEqual(error['code'], code)
                self.assertIn(fragment, error['message'])

    def test_checked_bindings_and_control_flow(self):
        cases = [
            ("fn main(): Unit { val size = 2; size = 3 }", "size = 3", "LYT0004"),
            ("fn main(): Unit { print(unknown) }", "unknown", "LYN0001"),
            ("fn main(): Unit { val x = 1; val x = 2 }", "x = 2", "LYN0002"),
            ("fn f(x: Bool): Int { x } fn main(): Unit {}", "x }", "LYT0001"),
            ("fn main(): Unit { if 1 { print(0) } }", "1 {", "LYT0001"),
            ("fn main(): Unit { while false { val x = 0 }; print(x) }", "x)", "LYN0001"),
            ("fn main(): Unit { break }", "break", "LYT0005"),
            ("fn main(): Unit { val x = 1 < 2 == true }", "==", "LYP0001"),
            ("fn f(): Int { return\n42 } fn main(): Unit {}", "return", "LYT0001"),
            ("fn main(num: Int): Unit {}", "main", "LYT0002"),
        ]
        for source, marker, code in cases:
            with self.subTest(source=source):
                self.main.write_text(source)
                result = self.cli("check", "--diagnostics=json")
                self.assertNotEqual(result.returncode, 0)
                error, = json.loads(result.stderr)
                self.assertEqual((error["code"], error["range"]["start"]["offset"]),
                                 (code, source.index(marker)))

    def test_cc001_check(self):
        self.fixture("fail")
        diagnostic = self.semicolon_error(self.cli("check", "--diagnostics=json"))
        text = self.cli("check")
        self.assertNotEqual(text.returncode, 0)
        self.assertIn("LYT0001", text.stderr)
        self.assertIn("Int", text.stderr)
        self.assertIn("Unit", text.stderr)
        start = diagnostic["range"]["start"]
        self.assertIn(f'{start["line"]}:{start["column"]}', text.stderr)
        self.assertIn(diagnostic["hint"], text.stderr)

    def test_cc002_check_without_build_tools(self):
        self.fixture("pass")
        result = self.cli("check", env={**os.environ, "PATH": ""})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.package / "build").exists())

    def test_crlf_and_utf8_location(self):
        self.main.write_bytes("// 注释\r\nfn total(): Int { /* 加法 */ 1 + 2; }\r\n"
                              "fn main(): Unit { print(total()) }\r\n".encode())
        self.semicolon_error(self.cli("check", "--diagnostics=json"))

    def test_check_rejects_bad_programs(self):
        for source in ("fn main(): Unit { print(missing()) }", "fn main(): Unit { print() }",
                       "fn main(): Unit { print(1, 2) }", "fn main(): Unit { print(print(1)) }",
                       "fn main(): Unit { print(9223372036854775808) }", "fn main(): Int32 { 1 }",
                       "fn f(): Int { 1 }", "fn main(): Unit {} fn main(): Unit {}",
                       "fn main(): Unit { print(1 }", "fn main(): Unit { 1\n+ 2; }"):
            with self.subTest(source=source):
                self.main.write_text(source)
                result = self.cli("check", "--diagnostics=json")
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(json.loads(result.stderr)[0]["code"])


class Native(PackageTest):
    def test_main_int_returns_after_cleanup(self):
        for prefix, body in (('', ''), ('async ', 'await sleepMs(0); ')):
            for code in (0, 2, 255, -1, 256):
                with self.subTest(prefix=prefix, code=code):
                    self.main.write_text(f'{prefix}fn main(): Int {{ defer print("cleanup"); {body}{code} }}')
                    result = self.cli('run')
                    self.assertEqual(result.stdout, 'cleanup\n')
                    if 0 <= code <= 255:
                        self.assertEqual((result.returncode, result.stderr), (code, ''))
                    else:
                        self.assertEqual(result.returncode, 1)
                        self.assertIn('expected an exit code in 0..255', result.stderr)

    def test_unavailable_or_corrupt_object_cache_does_not_break_build(self):
        self.fixture('pass')
        cache = self.package / '对象 cache'

        def run(cache_path):
            result = self.cli('run', '--release', env={**os.environ, 'LYRA_CACHE': str(cache_path)})
            self.assertEqual((result.returncode, result.stdout, result.stderr), (0, '3\n', ''))

        run(self.main)
        run(cache)
        entry, = (cache / 'compiler' / 'objects').iterdir()
        truncated = entry.read_bytes()[:-8]
        entry.write_bytes(truncated)
        run(cache)
        self.assertNotEqual(entry.read_bytes(), truncated)

    def test_fixed_width_integers_floats_and_checked_conversions(self):
        self.main.write_text('''
fn flip(flag: Bool): Int8 { if flag { 1 + 2 } else { -128 } }
fn identity<T>(value: T): T { value }
fn choose<T>(left: T, right: T): T { right }
fn main(): Unit {
    print(flip(false))
    val small: UInt8 = 0xff
    val large: UInt64 = 0xffff_ffff_ffff_ffff
    print(large)
    print(large % 7)
    print(match large { 18446744073709551615 => 1, _ => 0 })
    val signed: Int16 = -1000
    print(small + signed)
    print(choose(small, signed))
    val alias: Int64 = signed
    print(identity<Int>(alias))
    print(match large.toIntChecked() { None => 2, _ => 0 })
    print((-1).toUInt8Checked() ?? 3)
    print((256).toUInt8Checked() ?? 4)
    print((255).toUInt8Checked() ?? 0)
    val a: Float32 = 16_777_217
    print(a)
    var value: Float = 1 + 3.5e+0
    value *= 2
    value %= 2.5
    print(value)
    val widened: Float64 = 3.25f32
    print(identity(widened))
    print(large.toFloat())
    print((-128.0).toInt8Checked() ?? 0)
    print((127.5).toInt8Checked() ?? 5)
    print((128.0).toInt8Checked() ?? 6)
    print((18446744073709551616.0).toUIntChecked() ?? 7)
    print((9223372036854775808.0).toUIntChecked() ?? 0)
    val zero: Float = 0
    val nan = zero / zero
    if nan != nan && !(nan <= 0) && !(nan >= 0) { print(8) }
    print(nan.toIntChecked() ?? 9)
    print(1 + 2.5)
    print(1.25f32 + 1.25f64)
}
''', encoding='utf-8')
        expected = ('-128\n18446744073709551615\n1\n1\n-745\n-1000\n-1000\n2\n3\n4\n255\n'
                    '16777216\n1.5\n3.25\n1.8446744073709552e+19\n-128\n5\n6\n7\n9223372036854775808\n8\n9\n3.5\n2.5\n')
        for flags in ([], ['--release']):
            with self.subTest(flags=flags):
                output = executable(self.package / 'numbers')
                result = self.cli('build', '-o', output, *flags)
                self.assertEqual(result.returncode, 0, result.stderr)
                run = subprocess.run([output], capture_output=True, text=True, timeout=5)
                self.assertEqual((run.returncode, run.stdout, run.stderr), (0, expected, ''))

    def test_small_and_unsigned_arithmetic_traps_in_both_profiles(self):
        for declaration, operation, fragment in (
            ('val value: UInt8 = 255', 'value + 1', 'UInt8 overflow'),
            ('val value: UInt64 = 0', 'value - 1', 'UInt overflow'),
            ('val value: Int8 = -128', 'value / -1', 'Int8 overflow'),
            ('val value: Int16 = -32768', '-value', 'Int16 overflow'),
            ('val value: UInt16 = 2', 'value % 0', 'UInt16 division by zero'),
        ):
            self.main.write_text(f'fn main(): Unit {{ {declaration}; print({operation}) }}', encoding='utf-8')
            for flags in ([], ['--release']):
                with self.subTest(operation=operation, flags=flags):
                    output = executable(self.package / 'numbers')
                    result = self.cli('build', '-o', output, *flags)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    run = subprocess.run([output], capture_output=True, text=True, timeout=5)
                    self.assertNotEqual(run.returncode, 0)
                    self.assertIn(fragment, run.stderr)
                    self.assertIn('main.ly:1:', run.stderr)

    def test_numeric_c_abi(self):
        source = self.package / 'numbers.c'
        source.write_text('''
#include <stdint.h>
int8_t low(int8_t value) { return value; }
uint16_t high(uint16_t value) { return value; }
float half(float value) { return value / 2.0f; }
double sum(double value, uint64_t offset) { return value + (double)(offset % 5); }
''', encoding='utf-8')
        library = c_archive(source, self.package / 'numbers')
        self.main.write_text('''
extern fn low(value: Int8): Int8;
extern fn high(value: UInt16): UInt16;
extern fn half(value: Float32): Float32;
extern fn sum(value: Float64, offset: UInt64): Float64;
fn main(): Unit {
    print(low(-128))
    print(high(65535))
    print(half(3.5))
    print(sum(2.5, 18446744073709551613))
}
''', encoding='utf-8')
        output = executable(self.package / 'ffi')
        result = self.cli('build', '--release', '--link', library, '-o', output)
        self.assertEqual(result.returncode, 0, result.stderr)
        run = subprocess.run([output], capture_output=True, text=True, timeout=5)
        self.assertEqual((run.returncode, run.stdout, run.stderr), (0, '-128\n65535\n1.75\n5.5\n', ''))

    def test_loop_branches_and_evaluation_order(self):
        self.main.write_text('''
fn sum(limit: Int): Int {
    var index = 0
    var total: Int = 0
    while true {
        index += 1
        if index > limit { break }
        if index % 2 == 0 { continue }
        total = total + index
    }
    total
}
fn mark(value: Int): Int { print(value); value }
fn diff(left: Int, right: Int): Int { left - right }
fn noisy(): Bool { print(999); true }
fn nested(flag: Bool): Int {
    if flag { return 3; } else { return 4; }
}
fn main(): Unit {
    print(sum(6))
    print(diff(mark(1), mark(2)))
    if false && noisy() || !true { print(999) }
    print(if true || noisy() { nested(false) * -2 } else { 0 })
    val outer = 5
    { val outer = 6; print(outer) }
    print(outer)
}
''')
        for flags in ([], ["--release"]):
            output = executable(self.package / "app")
            result = self.cli("build", "-o", output, *flags)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.check_output([output], timeout=5), b"9\n1\n2\n-1\n-8\n6\n5\n")

    def test_invalid_integer_operations(self):
        for expression in ("-(-9223372036854775808)", "-9223372036854775808 - 1",
                           "9223372036854775807 * 2", "-9223372036854775808 / -1", "1 / 0", "1 % 0"):
            self.main.write_text(f"fn main(): Unit {{ print({expression}) }}")
            for flags in ([], ["--release"]):
                with self.subTest(expression=expression, flags=flags):
                    output = executable(self.package / "app")
                    result = self.cli("build", "-o", output, *flags)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    run = subprocess.run([output], capture_output=True, text=True, timeout=5)
                    self.assertGreater(run.returncode, 0)
                    self.assertIn("main.ly:1:", run.stderr)

    def test_cc001_no_output(self):
        self.fixture("fail")
        output = self.package / "out"
        self.semicolon_error(self.cli("build", "--diagnostics=json", "-o", output,
                                      env={**os.environ, "PATH": ""}))
        self.assertFalse(output.exists())
        output.write_bytes(b"previous artifact")
        self.semicolon_error(self.cli("build", "--diagnostics=json", "-o", output))
        self.assertEqual(output.read_bytes(), b"previous artifact")

    def test_cc002_executable(self):
        self.fixture("pass")
        for profile, flags in (("debug", []), ("release", ["--release"])):
            with self.subTest(profile=profile):
                result = self.cli("build", *flags)
                self.assertEqual(result.returncode, 0, result.stderr)
                output = executable(self.package / "build" / profile / "app")
                run = subprocess.run([output], capture_output=True, timeout=5)
                self.assertEqual((run.returncode, run.stdout, run.stderr), (0, b"3\n", b""))
                assert_linkage(self, output)

    def test_same_module_forward_calls_and_block_values(self):
        self.main.write_text("fn main(): Unit { print(answer()) }\n")
        (self.src / "z.ly").write_text("fn answer(): Int { print(1);\n (40 +\n 2)\n }\n")
        nested = self.src / "other"
        nested.mkdir()
        (nested / "bad.ly").write_text("not in this module")
        output = executable(self.package / "新目录" / "my app")
        result = self.cli("build", "-o", output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(subprocess.check_output([output], timeout=5), b"1\n42\n")

    def test_overflow_checked_in_both_profiles(self):
        for value, delta in (("9223372036854775807", "1"), ("-9223372036854775808", "-1")):
            self.main.write_text(f"fn limit(): Int {{ {value} }}\nfn main(): Unit {{ print(limit() + {delta}) }}")
            for flags in ([], ["--release"]):
                with self.subTest(value=value, flags=flags):
                    output = executable(self.package / "overflow")
                    result = self.cli("build", "-o", output, *flags)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    run = subprocess.run([output], capture_output=True, text=True, timeout=5)
                    self.assertNotEqual(run.returncode, 0)
                    self.assertEqual(run.stdout, "")
                    self.assertIn("overflow", run.stderr.lower())
                    self.assertIn("main.ly:2:", run.stderr)

    def test_link_failure_preserves_old_output(self):
        self.main.write_text('extern fn missingNativeFunction(): Int;\nfn main(): Unit { print(missingNativeFunction()) }\n')
        output = self.package / "existing"
        output.write_bytes(b"previous artifact")
        before = set(self.package.iterdir())
        result = self.cli("build", "--diagnostics=json", "-o", output)
        self.assertNotEqual(result.returncode, 0)
        diagnostic, = json.loads(result.stderr)
        self.assertEqual(diagnostic["code"], "LYB0002")
        self.assertIn("missingNativeFunction", diagnostic["message"])
        self.assertEqual(output.read_bytes(), b"previous artifact")
        self.assertEqual(set(self.package.iterdir()), before)

    @unittest.skipUnless(WINDOWS, 'Windows keeps a loaded executable mapped')
    def test_failed_rebuild_preserves_the_running_executable(self):
        (self.package / 'lyra.json5').write_text('{name:"reload",version:"1.0.0"}')
        self.main.write_text('import std.io.{readLine}\nfn main(): Unit { print(1); val _ = readLine(); print(2) }')
        output = executable(self.package / 'running')
        built = self.cli('build', '-o', output)
        self.assertEqual(built.returncode, 0, built.stderr)
        before = output.read_bytes()
        with running([output]) as (process, stream):
            self.assertEqual(stream.line(), b'1\n')
            self.main.write_text('fn main(): Unit { print(3) }')
            blocked = self.cli('build', '-o', output, '--diagnostics=json')
            self.assertNotEqual(blocked.returncode, 0)
            self.assertEqual(json.loads(blocked.stderr)[0]['code'], 'LYI0001')
            self.assertEqual(output.read_bytes(), before)
            process.stdin.write(b'continue\n')
            process.stdin.flush()
            self.assertEqual(stream.line(), b'2\n')
            self.assertEqual(process.wait(timeout=5), 0)
        rebuilt = self.cli('build', '-o', output)
        self.assertEqual(rebuilt.returncode, 0, rebuilt.stderr)
        self.assertEqual(subprocess.check_output([output], timeout=5), b'3\n')


if __name__ == "__main__":
    unittest.main()
