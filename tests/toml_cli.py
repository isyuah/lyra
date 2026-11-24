import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class Toml(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.package = Path(temporary.name)
        self.main = self.package / 'src/main.ly'
        self.main.parent.mkdir()
        (self.package / 'lyra.json5').write_text('{name:"toml-example", version:"0.1.0"}')
        self.env = {**os.environ, 'LYRA_CACHE': str(self.package / 'cache')}

    def command(self, source, command, *args):
        self.main.write_text(source, encoding='utf-8')
        return subprocess.run([COMPILER, command, self.package, *args], capture_output=True, env=self.env, timeout=45)

    def test_real_tables_dotted_keys_arrays_and_roundtrip(self):
        source = r'''#derive(Toml)
struct Host(pub val name: String, pub val port: UInt16 = 8080)
#derive(Toml)
struct Config(pub val title: String, pub val active: Bool, pub val rate: Float32, pub val host: Host,
              pub val mirrors: List<Host>, pub val matrix: List<List<Int>>, pub val note: String? = None,
              val internal: String = title, [tomlName("app.label")] pub val label: String = internal)
fn runConfig(text: String): Result<String,String> {
    val config = decodeConfigToml(text)?
    val again = decodeConfigToml(encodeConfigToml(config))?
    Ok("${again.title}\n${again.active}\n${again.rate}\n${again.host.name}:${again.host.port}\n${again.mirrors[1].name}:${again.mirrors[1].port}\n${again.matrix[1][0]}\n${again.label}\n${again.note == None}")
}
fn main(): Unit {
    val input = "title = \"demo\" # real TOML\nactive = true\nrate = 1.25e1\nhost.name = 'local'\nmatrix = [[0x2A, 3], [1_000]]\n[[mirrors]]\nname = 'one'\n[[mirrors]]\nname = \"two\"\nport = 443\n"
    print(match runConfig(input) { Ok(text) => text, Err(error) => error })
}'''
        for flags in ([], ['--release']):
            with self.subTest(flags=flags):
                result = self.command(source, 'run', *flags)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertEqual(result.stdout.decode().splitlines(), ['demo', 'true', '12.5', 'local:8080',
                                                                       'two:443', '1000', 'demo', 'true'])

    def test_strict_paths_missing_defaults_and_parser_errors(self):
        source = r'''import std.toml.parseToml
#derive(Toml)
struct Record([tomlName("team.name")] pub val name: String, pub val num: Int8)
#derive(Toml)
struct Batch(pub val records: List<Record>, pub val active: Bool = true)
fn show(text: String, loose: Bool): String {
    val result = if loose { decodeBatchTomlIgnoringUnknownFields(text) } else { decodeBatchToml(text) }
    match result { Ok(batch) => batch.records[0].name, Err(error) => error }
}
fn main(): Unit {
    val row = "[[records]]\n\"team.name\" = 'A'\nnum = 7\nupdated = 1979-05-27T07:32:00Z"
    print(show(row, false)); print(show(row, true))
    print(show("records = [{num = 7}]", false))
    print(show("records = [{\"team.name\" = 'A', num = 256}]", true))
    print(show("records = 1", false))
    print(match parseToml("x=1\nx=2") { Ok(_) => "accepted", Err(_) => "duplicate rejected" })
    print(match parseToml("x = null") { Ok(_) => "accepted", Err(_) => "null rejected" })
    print(match parseToml("x = 1\0trailing") { Ok(_) => "accepted", Err(_) => "NUL rejected" })
}'''
        result = self.command(source, 'run')
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(result.stdout.decode().splitlines(), ['$.records[0].updated: unknown field', 'A',
                         '$.records[0]["team.name"]: expected String', '$.records[0].num: out of range for Int8',
                         '$.records: expected array', 'duplicate rejected', 'null rejected', 'NUL rejected'])

    def test_scalar_roundtrip_and_ambiguous_none(self):
        source = r'''#derive(Toml)
struct Values(pub val text: String, pub val amount: Float, pub val port: UInt, pub val tag: String? = "fallback")
fn main(): Unit {
    val input = Values("mid\0\u{7f}\t\n\"\\🙂", -0.0, 17)
    val output = encodeValuesToml(input)
    print(match decodeValuesToml(output) { Ok(v) => v.text == input.text && (1.0 / v.amount) < 0.0 && v.port == 17 && (v.tag ?? "") == "fallback", Err(_) => false })
    val zero: Float = 0.0
    print(match decodeValuesToml(encodeValuesToml(Values("inf", 1.0 / zero, 0))) { Ok(v) => v.amount > 1e300, Err(_) => false })
    print(try { encodeValuesToml(Values("", 0.0, 0, None)) } catch error: Exception { error.message })
    print(try { encodeValuesToml(Values("", 0.0, 18446744073709551615)) } catch error: Exception { error.message })
}'''
        result = self.command(source, 'run')
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(result.stdout.decode().splitlines(), ['true', 'true', '$.tag: cannot omit None with a custom TOML default',
                                                              '$.port: TOML integer exceeds Int64'])

    def test_unrepresentable_types_rejected_at_derive(self):
        for declaration, message in [('struct Bad(pub val data: Option<String?>)', 'no null'),
                                     ('struct Bad(pub val data: List<Int?>)', 'no null'),
                                     ('struct Bad(val id: Int)', 'private TOML field'),
                                     ('class Bad(pub val id: Int)', 'expects a struct')]:
            with self.subTest(declaration=declaration):
                result = self.command('#derive(Toml)\n' + declaration + '\nfn main(): Unit {}', 'check', '--diagnostics=json')
                self.assertNotEqual(result.returncode, 0)
                error, = json.loads(result.stderr)
                self.assertEqual(error['code'], 'LYM0002', error)
                self.assertIn(message, error['message'])


if __name__ == '__main__':
    unittest.main()
