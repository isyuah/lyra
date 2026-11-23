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
        self.assertEqual((Path(error['file']), error['range']['start']['line'], error['range']['start']['column']), (main, 1, 2))
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

    def test_derive_fixed_width_integers_check_the_wire_value(self):
        types = ('Int8', 'Int16', 'Int32', 'Int64', 'UInt8', 'UInt16', 'UInt32')
        names = ('a', 'b', 'c', 'd', 'e', 'f', 'g')
        minima = (-128, -32768, -2147483648, -9223372036854775808, 0, 0, 0)
        maxima = (127, 32767, 2147483647, 9223372036854775807, 255, 65535, 4294967295)
        code = '#derive(Json)\nstruct Widths(' + ', '.join(f'pub val {name}: {kind}' for name, kind in zip(names, types)) + ')\n'
        code += '#derive(Json)\nstruct Packet(pub val rows: List<List<UInt8?>>)\n'
        code += 'fn show(text: String): String { match decodeWidths(text) { Ok(value) => encodeWidths(value), Err(error) => error } }\nfn main(): Unit {\n'
        expected = []

        def trial(values, error=None):
            wire = json.dumps(dict(zip(names, values)), separators=(',', ':'))
            expected.append(wire if error is None else error)
            return 'print(show(' + json.dumps(wire) + '))\n'

        code += trial(minima) + trial(maxima)
        for index, kind in enumerate(types):
            if kind != 'Int64':
                for rejected in (minima[index] - 1, maxima[index] + 1):
                    values = list(maxima)
                    values[index] = rejected
                    code += trial(values, f'$.{names[index]}: out of range for {kind}')
            values = list(minima)
            values[index] = 1.0
            code += trial(values, f'$.{names[index]}: expected {kind}')
        code += r'print(match decodePacket("{\"rows\":[[],[0,null,256]]}") { Ok(data) => encodePacket(data), Err(error) => error })'
        self.native(code + '}', expected + ['$.rows[1][2]: out of range for UInt8'])

    def test_derive_floats_roundtrip_and_report_nested_failures(self):
        self.native(r'''import std.json.parseJson
#derive(Json)
struct Measure([jsonName("wide-real")] pub val wide: Float, pub val entries: List<List<Float32?>>, pub val extra: Option<Float64?>)
fn show(text: String): String { match decodeMeasure(text) { Ok(value) => encodeMeasure(value), Err(error) => error } }
fn invalid(text: String): String { match parseJson(text) { Ok(_) => "accepted", Err(_) => "parse rejected" } }
fn main(): Unit {
    print(show("{\"wide-real\":-0.0,\"entries\":[[0.125,null],[2]],\"extra\":null}"))
    print(show("{\"wide-real\":1.25e2,\"entries\":[[]]}"))
    print(show("{\"wide-real\":17,\"entries\":[],\"extra\":0.12345678901234568}"))
    print(show("{\"wide-real\":true,\"entries\":[]}"))
    print(show("{\"wide-real\":1,\"entries\":[[null,\"3.5\"]]}"))
    print(show("{\"wide-real\":1,\"entries\":[[],[1e40]]}"))
    print(show("{\"wide-real\":0,\"entries\":[[-1e-80]]}"))
    val binary32: Float32 = 0.1
    val sample = Measure(0.12345678901234568, [[Some(binary32)]], None)
    match decodeMeasure(encodeMeasure(sample)) {
        Ok(again) => { if again.wide == sample.wide && (again.entries[0][0] ?? 0) == binary32 { print("float roundtrip") } },
        Err(error) => print(error),
    }
    val input = "{\"wide-real\":9007199254740993,\"entries\":[[4611686293305294849]],\"extra\":5e-324}"
    match decodeMeasure(input) {
        Ok(value) => {
            val number: Int = 4611686293305294849
            val item: Float32 = value.entries[0][0] ?? 0
            if value.wide == 9007199254740992.0 && item == number.toFloat32() && item > 4611686018427387904.0f32 { print("integer conversion") }
            match decodeMeasure(encodeMeasure(value)) {
                Ok(again) => { val tiny: Float = (again.extra ?? None) ?? 0; if tiny > 0 { print("subnormal survives") } },
                Err(error) => print(error),
            }
        }, Err(error) => print(error),
    }
    val zero: Float = 0
    val infinity = 1.0 / zero
    print(try { encodeMeasure(Measure(infinity, [], None)) } catch error: Exception { error.message })
    print(try { encodeMeasure(Measure(0, [[Some((-infinity).toFloat32())]], None)) } catch error: Exception { error.message })
    print(try { encodeMeasure(Measure(0, [], Some(Some(zero / zero)))) } catch error: Exception { error.message })
    print(invalid("9223372036854775808")); print(invalid("1e400")); print(invalid("NaN"))
}''', ['{"wide-real":-0.0,"entries":[[0.125,null],[2.0]],"extra":null}', '{"wide-real":125.0,"entries":[[]]}',
       '{"wide-real":17.0,"entries":[],"extra":0.12345678901234568}', '$["wide-real"]: expected Float64',
       '$.entries[0][1]: expected Float32', '$.entries[1][0]: out of range for Float32', '{"wide-real":0.0,"entries":[[-0.0]]}',
       'float roundtrip', 'integer conversion', 'subnormal survives',
       'JSON cannot encode a non-finite Float64', 'JSON cannot encode a non-finite Float32', 'JSON cannot encode a non-finite Float64',
       'parse rejected', 'parse rejected', 'parse rejected'])

    def test_json_uint64_requires_an_explicit_representation(self):
        for annotation in ('UInt', 'List<UInt64?>'):
            with self.subTest(annotation=annotation):
                (self.src / 'main.ly').write_text('#derive(Json)\nstruct Bad(pub val value: ' + annotation + ')\nfn main(): Unit {}')
                self.diagnostic('LYM0002', 'UInt/UInt64')

    def test_nested_arrays_track_field_and_element_paths(self):
        self.native(r'''#derive(Json)
struct Report(pub val rows: List<List<Int?>>, pub val labels: List<String>?)
fn show(input: String): String { match decodeReport(input) { Ok(report) => encodeReport(report), Err(reason) => reason } }
fn main(): Unit {
    print(show("{\"rows\":[[1,null],[],[2]],\"labels\":[\"a\",\"b\"]}"))
    print(show("{\"rows\":[]}"))
    print(show("{\"rows\":[[1,true]]}"))
}''', ['{"rows":[[1,null],[],[2]],"labels":["a","b"]}', '{"rows":[]}', '$.rows[0][1]: expected Int'])

    def test_json_name_roundtrips_nested_lists_and_keeps_wire_paths(self):
        self.native(r'''#derive(Json)
struct Person([jsonName("display-name")] pub val name: String, [jsonName("nick.name")] pub val nick: Option<String?>)
#derive(Json)
struct Group([jsonName("$people")] pub val members: List<Person>)
fn show(input: String): String { match decodeGroup(input) { Ok(group) => encodeGroup(group), Err(reason) => reason } }
fn main(): Unit {
    print(show(encodeGroup(Group([Person("Ada", None), Person("Bo", Some(None)), Person("Cy", Some(Some("C")))]))))
    print(show("{\"$people\":[{\"display-name\":\"Ada\"},{\"display-name\":false}]}"))
    print(show("{\"$people\":[{\"name\":\"Ada\"}]}"))
    print(show("{\"$people\":[{\"odd.key\":5}]}"))
    print(show("{\"$people\":[{\"display-name\":\"Ada\",\"nick.name\":false}]}"))
    print(show("{\"$people\":[{}]}"))
    print(show("{\"members\":[]}"))
}''', ['{"$people":[{"display-name":"Ada"},{"display-name":"Bo","nick.name":null},{"display-name":"Cy","nick.name":"C"}]}',
       '$["$people"][1]["display-name"]: expected String', '$["$people"][0].name: unknown field',
       '$["$people"][0]["odd.key"]: unknown field', '$["$people"][0]["nick.name"]: expected String',
       '$["$people"][0]["display-name"]: expected String', '$.members: unknown field'])

    def test_json_keys_use_json_escaping_for_output_and_paths(self):
        key = '${rate}\\"\n品'
        wire = json.dumps(key, ensure_ascii=False)
        self.native(r'''#derive(Json)
struct Odd([jsonName("\${rate}\\\"\n品")] pub val value: Int)
fn show(input: String): String { match decodeOdd(input) { Ok(item) => encodeOdd(item), Err(reason) => reason } }
fn main(): Unit {
    print(show(encodeOdd(Odd(4))))
    print(show("{}"))
    print(show("{\"bad\\\"key\":0}"))
    print(show("{\"\":0}"))
}''', ['{' + wire + ':4}', '$[' + wire + ']: expected Int', '$["bad\\"key"]: unknown field', '$[""]: unknown field'])

    def test_json_name_rejects_ambiguous_or_unrepresentable_keys(self):
        main = self.src / 'main.ly'
        for fields, fragment in (
            ('pub val id: Int, [jsonName("id")] pub val code: Int', 'Duplicate JSON key "id"'),
            ('[jsonName("id")] pub val code: Int, pub val id: Int', 'Duplicate JSON key "id"'),
            ('[jsonName("same")] pub val one: Int, [jsonName("same")] pub val two: Int', 'Duplicate JSON key "same"'),
            ('[jsonName] pub val field: Int', 'non-empty key'),
            ('[jsonName("")] pub val field: Int', 'non-empty key'),
            (r'[jsonName("before\0after")] pub val field: Int', 'without NUL'),
        ):
            with self.subTest(fields=fields):
                main.write_text('#derive(Json)\nstruct Bad(' + fields + ')\nfn main(): Unit {}', encoding='utf-8')
                error = self.diagnostic('LYM0002', fragment)
                self.assertEqual((Path(error['file']), error['range']['start']['line']), (main, 1))

    def test_macro_contains_matches_exact_substrings(self):
        self.macro('if macroContains("ab\\0cd", "\\0c") && macroContains("abc", "") && !macroContains("Case", "case") && !macroContains("\u00e9", "e\u0301") { "" } else { macroFail("substring comparison lost data") }')
        (self.src / 'main.ly').write_text('#make\nfn main(): Unit {}', encoding='utf-8')
        result = self.cli('check', self.pkg)
        self.assertEqual(result.returncode, 0, result.stderr.decode())

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
fn first<A,B>(first: A, second: B): A { first }
fn main(): Unit {
    val temp = 2
    print(2 * #make(true, add(temp, 1)))
    print(#make(false, add(temp, 9)))
    print(#make(true, #make(true, temp)))
    print(#make(temp < 3, first<Int,List<Int>>(4, [1, 2])))
}''', ['add', '26', '0', '22', '14'])
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
