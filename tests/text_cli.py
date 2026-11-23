import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from platform_cli import assert_linkage, executable


COMPILER = Path(sys.argv.pop(1)).resolve()


class Text(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.package = Path(directory.name)
        (self.package / 'src').mkdir()
        self.source = self.package / 'src/main.ly'

    def native(self, source, expected):
        self.source.write_bytes(source.encode())
        for profile, flags in (('debug', []), ('release', ['--release'])):
            with self.subTest(profile=profile):
                app = executable(self.package / profile)
                build = subprocess.run([COMPILER, 'build', self.package, *flags, '-o', app], capture_output=True, timeout=30)
                self.assertEqual(build.returncode, 0, build.stderr.decode())
                result = subprocess.run([app], capture_output=True, timeout=10)
                self.assertEqual((result.returncode, result.stdout, result.stderr), (0, expected.encode(), b''))
                assert_linkage(self, app)

    def diagnostic(self, source, code, fragment):
        self.source.write_text(source, encoding='utf-8')
        result = subprocess.run([COMPILER, 'check', self.package, '--diagnostics=json'], capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        diagnostic, = json.loads(result.stderr)
        self.assertEqual(diagnostic['code'], code, diagnostic)
        self.assertIn(fragment, diagnostic['message'])
        return diagnostic['range']

    def test_literals_scalar_indexing_and_lossless_byte_conversion(self):
        self.native(r'''
fn main(): Unit {
    val text = "A\u{E9}🙂\0"
    print(text.size)
    print(text.byteSize)
    print(text[2])
    print(text[1].codePoint)
    print('🙂' == '\u{1F642}')
    print(match '\'' { '\'' => "quote", _ => "unexpected" })
    print(r#"\n "${raw}""#)
    val bytes = b"\xFF\0"
    print(bytes.size)
    print(bytes[0])
    print(bytes.toString() ?? "invalid UTF-8")
    print(text.toBytes().toString() ?? "failed")
    print(br#"\xFF"# == b"\\xFF")
    print(r#"first
second"#)
}
''', '4\n8\n🙂\n233\ntrue\nquote\n\\n "${raw}"\n2\n255\ninvalid UTF-8\nAé🙂\0\ntrue\nfirst\nsecond\n')

    def test_interpolation_uses_display_and_preserves_order_across_await(self):
        self.native(r'''
class Label(val message: String) {
    fn toString(): String { message }
}
trait Display { fn toString(): String }
fn format<T: Display>(item: T): String { "[${item}]" }
fn number(value: Int): Int { print(value); value }
async fn token(): Char { await blocking { gcCollect() }; '}' }
async fn main(): Unit {
    val mark = Label("Lyra")
    print("a${number(1)} ${if true { "nested=${format(mark)}" } else { "bad" }} ${await token()} ${number(2)}")
    print("\${name} ${1.25f32} ${false}")
    for item in "é中🙂" {
        if item == '中' { continue }
        await blocking { gcCollect() }
        print(item)
    }
    for byte in b"\x80\xFF" { print(byte) }
}
''', '1\n2\na1 nested=[Lyra] } 2\n${name} 1.25 false\né\n🙂\n128\n255\n')

    def test_expression_macro_preserves_interpolation_and_raw_literals(self):
        (self.package / 'lyra.json5').write_text("{name:'text', version:'1.0.0'}")
        macros = self.package / 'src/.expressions'
        macros.mkdir()
        (macros / 'wrap.ly').write_text('''
fn wrap(target: String, arguments: String): String {
    if argumentCount() != 2 { return macroFail("expected two arguments") }
    "${macroQuote(argumentString(1))} + ${argumentCode(0)}"
}
''')
        self.native(r'''
fn pair(first: Int, second: Int): Int { first + second }
fn main(): Unit { print("outer: ${#wrap("x=${pair(1, 2)}", r#"\prefix "#)}") }
''', 'outer: \\prefix x=3\n')

    def test_rejections_and_source_ranges(self):
        for expression, code, fragment in (
            ("'é'", 'LYL0003', '恰好'),
            (r"'\u{d800}'", 'LYL0003', 'Unicode'),
            (r'"\xFF"', 'LYL0003', '字节串'),
            (r'b"\xG0"', 'LYL0003', '十六进制'),
            ('r##"missing"#', 'LYL0003', '分隔符'),
            ('"${1', 'LYL0003', '插值'),
        ):
            with self.subTest(expression=expression):
                self.diagnostic('fn main(): Unit { val value = ' + expression, code, fragment)
        self.diagnostic('fn main(): Unit { val bytes = b"a"; bytes[0] = 42 }', 'LYT0004', '赋值')
        source = 'class View()\nfn main(): Unit { val bad = View(); print("前缀 ${bad}") }'
        site = self.diagnostic(source, 'LYT0016', 'Display')
        self.assertEqual(self.source.read_bytes()[site['start']['offset']:site['end']['offset']], b'bad')

    def test_index_error_reports_the_operator(self):
        for value in ('"é"', 'b"A"'):
            with self.subTest(value=value):
                self.source.write_text('fn main(): Unit { print(' + value + '[1]) }', encoding='utf-8')
                app = executable(self.package / ('invalid-text' if value[0] == '"' else 'invalid-bytes'))
                build = subprocess.run([COMPILER, 'build', self.package, '-o', app], capture_output=True, timeout=30)
                self.assertEqual(build.returncode, 0, build.stderr.decode())
                run = subprocess.run([app], capture_output=True, timeout=10)
                self.assertNotEqual(run.returncode, 0)
                self.assertIn(b':1:' + str(25 + len(value)).encode() + b':', run.stderr)
                self.assertIn(b'index 1 out of bounds (size 1)', run.stderr)


if __name__ == '__main__':
    unittest.main()
