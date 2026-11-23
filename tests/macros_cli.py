import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class Macros(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.pkg = self.root / 'example'
        self.src = self.pkg / 'src'
        (self.src / '.macros').mkdir(parents=True)
        self.manifest()

    def manifest(self, **settings):
        (self.pkg / 'lyra.json5').write_text(json.dumps({'name': 'example', 'version': '0.1.0', **settings}))

    def cli(self, *args):
        return subprocess.run([COMPILER, *args], capture_output=True, timeout=40,
                              env={**os.environ, 'LYRA_CACHE': str(self.root / 'cache')})

    def native(self, source, lines):
        (self.src / 'main.ly').write_text(source)
        for options in ([], ['--release']):
            result = self.cli('run', self.pkg, *options)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertEqual(result.stdout.decode().splitlines(), lines)

    def diagnostic(self, code, message=''):
        result = self.cli('check', self.pkg, '--diagnostics=json')
        self.assertNotEqual(result.returncode, 0)
        error, = json.loads(result.stderr)
        self.assertEqual(error['code'], code, error)
        self.assertIn(message, error['message'])
        return error

    def macro(self, source):
        (self.src / '.macros/make.ly').write_text('fn make(target: String, argument: String): String { ' + source + ' }')

    def test_declared_input_and_cold_decorator(self):
        self.manifest(macroInputs={'make': {'greeting': 'inputs/message.txt'}})
        (self.pkg / 'inputs').mkdir()
        snapshot = self.pkg / 'inputs/message.txt'
        snapshot.write_text('你好"$')
        self.macro('"fn greeting(): String { " + macroQuote(macroInput("greeting")) + " }"')
        code = '''#make
@trace
async fn work(): String { greeting() }
async fn main(): Unit { val pending = work(); print("cold"); print(await pending); print(await pending) }'''
        self.native(code, ['cold', 'trace: work', '你好"$', '你好"$'])
        snapshot.write_text('updated')
        self.native(code, ['cold', 'trace: work', 'updated', 'updated'])
        overwritten = self.cli('build', self.pkg, '-o', snapshot)
        self.assertNotEqual(overwritten.returncode, 0)
        self.assertEqual(snapshot.read_text(), 'updated')
        packed = self.cli('pack', self.pkg, '-o', self.root / 'example.lypkg')
        self.assertEqual(packed.returncode, 0, packed.stderr.decode())
        self.assertIn(b'inputs/message.txt', (self.root / 'example.lypkg').read_bytes())

    def test_denials_limits_and_generated_error_locations(self):
        main = self.src / 'main.ly'
        main.write_text('#make\nfn main(): Unit {}')
        self.macro('macroInput("secret")')
        self.diagnostic('LYM0002', 'secret')
        self.manifest(macroInputs={'make': {'secret': '../secret'}})
        self.diagnostic('LYK0001', '宏输入')
        self.manifest(macroInputs={'make': {'secret': 'input.txt'}})
        self.diagnostic('LYM0002', 'secret')
        (self.root / 'outside').write_text('not a macro input')
        (self.pkg / 'input.txt').symlink_to(self.root / 'outside')
        self.diagnostic('LYM0002', 'secret')
        self.macro('while true {}; ""')
        self.diagnostic('LYM0002', '预算')
        self.macro('"pub fn surprise(): Int { 0 }"')
        self.diagnostic('LYM0002', '公开')
        self.macro('"fn wrong(): Int { false }"')
        error = self.diagnostic('LYT0001')
        self.assertEqual((error['file'], error['range']['start']['line'], error['range']['start']['column']), (str(main), 1, 2))
        self.assertIn('fn wrong', error['generated']['source'])
        (self.src / '.macros/make.ly').write_text('extern fn forbidden(): Int;\nfn make(target: String, argument: String): String { "" }')
        self.diagnostic('LYM0002', 'extern')

    def test_derive_strict_roundtrip_and_optional_states(self):
        code = r'''#derive(Json)
struct User(pub val id: Int, pub val name: String, pub val active: Bool)
#derive(Json)
struct Profile(pub val user: User, pub val note: Option<String?>)
fn intText(value: Int): Int { value }
fn render(input: String): String {
    match decodeProfile(input) { Ok(profile) => encodeProfile(profile), Err(reason) => reason }
}
fn main(): Unit {
    val user = User(7, "中\"\0", true)
    val outer = Profile(user, None)
    print(render(encodeProfile(outer)))
    print(render(encodeProfile(Profile(user, Some(None)))))
    print(render(encodeProfile(Profile(user, Some(Some("text"))))))
    print(render("{\"user\":{\"naem\":0}}"))
    print(render("{\"user\":{\"id\":7,\"name\":false,\"active\":true}}"))
    print(render("{}"))
    print(intText(11))
}'''
        user = '{"id":7,"name":"中\\\"\\u0000","active":true}'
        self.native(code, ['{"user":' + user + '}', '{"user":' + user + ',"note":null}',
                           '{"user":' + user + ',"note":"text"}', '$.user.naem: unknown field',
                           '$.user.name: expected String', '$.user: expected object', '11'])

    def test_derive_is_opt_in_for_every_type(self):
        main = self.src / 'main.ly'
        main.write_text('struct Item(pub val id: Int)\nfn main(): Unit { print(encodeItem(Item(7))) }')
        self.diagnostic('LYN0001', 'encodeItem')
        main.write_text('struct Item(pub val id: Int)\n#derive(Json)\nstruct Envelope(pub val item: Item)\nfn main(): Unit {}')
        error = self.diagnostic('LYN0001', 'encodeItem')
        self.assertEqual(error['range']['start']['line'], 2)
        self.assertIn('item', error['generated']['source'])


if __name__ == '__main__':
    unittest.main()
