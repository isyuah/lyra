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

    def test_nested_arrays_track_field_and_element_paths(self):
        self.native(r'''#derive(Json)
struct Report(pub val rows: List<List<Int?>>, pub val labels: List<String>?)
fn show(input: String): String { match decodeReport(input) { Ok(report) => encodeReport(report), Err(reason) => reason } }
fn main(): Unit {
    print(show("{\"rows\":[[1,null],[],[2]],\"labels\":[\"a\",\"b\"]}"))
    print(show("{\"rows\":[]}"))
    print(show("{\"rows\":[[1,true]]}"))
}''', ['{"rows":[[1,null],[],[2]],"labels":["a","b"]}', '{"rows":[]}', '$.rows[0][1]: expected Int'])

    def test_attributes_generate_a_typed_adapter_without_changing_direct_calls(self):
        (self.src / '.macros/post.ly').write_text('''fn post(target: String, route: String): String {
    if !fieldHasAttribute(0, "path") || fieldType(0) != "Int" { return macroFail("expected Int path") }
    val key = macroQuote(fieldAttributeValue(1, "query"))
    "fn adapt(request: Request): String { " + target + "(request." + fieldName(0) + ", request.query(" + key + ")) }\\nfn route(): String { " + macroQuote(route) + " }"
}''')
        self.native('''struct Request(val id: Int) { fn query(self, key: String): String { key + "!" } }
#post("/teams/{id}")
@trace
fn show([path] id: Int, [query("tag")] text: String): String { print(id); text }
fn main(): Unit { print(route()); print(adapt(Request(7))); print(show(8, "direct")) }''',
                    ['/teams/{id}', 'trace: show', '7', 'tag!', 'trace: show', '8', 'direct'])
        self.macro('"fn mappedName(): String { " + macroQuote(fieldAttributeValue(0, "key")) + " }"')
        self.native('#make\nstruct Envelope([key("external")] pub val name: String)\nfn main(): Unit { print(mappedName()) }', ['external'])
        (self.src / 'main.ly').write_text('fn work([path][path] id: Int): Int { id }\nfn main(): Unit {}')
        self.diagnostic('LYM0003', 'path')

    def test_expression_splices_preserve_scope_short_circuit_and_source_ranges(self):
        directory = self.src / '.expressions'
        directory.mkdir()
        expression = directory / 'make.ly'
        expression.write_text('''fn make(target: String, raw: String): String {
    if argumentCount() != 2 { return macroFail("expected two expressions") }
    "{ val temp = 10; if " + argumentCode(0) + " { temp + " + argumentCode(1) + " } else { 0 } }"
}''')
        self.native('''fn add(left: Int, right: Int): Int { print("add"); left + right }
fn main(): Unit {
    val temp = 2
    print(2 * #make(true, add(temp, 1)))
    print(#make(false, add(temp, 9)))
    print(#make(true, #make(true, temp)))
}''', ['add', '26', '0', '22'])
        main = self.src / 'main.ly'
        main.write_text('fn main(): Unit {\n    print(#make(true, false))\n}')
        error = self.diagnostic('LYT0001')
        self.assertEqual((error['range']['start']['line'], error['range']['start']['column']), (2, 23))
        expression.write_text('fn make(target: String, raw: String): String { "temp" }')
        main.write_text('fn main(): Unit { val temp = 7; print(#make()) }')
        error = self.diagnostic('LYN0001', 'temp')
        self.assertEqual(error['range']['start']['column'], main.read_text().index('#make') + 2)
        self.assertIn('temp', error['generated']['source'])

    def test_expression_macro_uses_only_declared_snapshot_and_one_expression(self):
        directory = self.src / '.expressions'
        directory.mkdir()
        impl = directory / 'query.ly'
        impl.write_text('''fn query(target: String, raw: String): String {
    if argumentString(0) != macroInput("statement") { return macroFail("refresh query snapshot") }
    "bindId(" + argumentCode(1) + ")"
}''')
        self.manifest(macroInputs={'query': {'statement': 'inputs/query.sql'}})
        (self.pkg / 'inputs').mkdir()
        (self.pkg / 'inputs/query.sql').write_text('SELECT ?1')
        code = 'fn bindId(id: Int): Int { id }\nfn main(): Unit { print(#query("SELECT ?1", 7)) }'
        self.native(code, ['7'])
        self.assertNotEqual(self.cli('build', self.pkg, '-o', impl).returncode, 0)
        main = self.src / 'main.ly'
        main.write_text(code.replace('SELECT ?1', 'SELECT ?2'))
        self.diagnostic('LYM0002', 'snapshot')
        main.write_text(code.replace('"SELECT ?1"', '7'))
        self.diagnostic('LYM0002', '字面量')
        main.write_text(code)
        for generated in ('1); pub fn escaped(): Int { 9 }', '#query(7)'):
            impl.write_text('fn query(target: String, raw: String): String { ' + json.dumps(generated) + ' }')
            self.diagnostic('LYM0002')
        main.write_text('fn main(): Unit { print(#derive(Json)) }')
        self.diagnostic('LYM0001', '表达式宏')


if __name__ == '__main__':
    unittest.main()
