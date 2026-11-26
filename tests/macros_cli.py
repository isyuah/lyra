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
        (self.src / '.macros/make.ly').write_text('fn fragment(target: String, argument: String): String { ' + source + ' }\n'
                                                  'fn make(target: String, argument: String): Syntax { syntaxCode(fragment(target, argument)) }')

    def library(self, name, **settings):
        package = self.root / name
        (package / 'src').mkdir(parents=True)
        (package / 'lyra.json5').write_text(json.dumps({'name': name, 'version': '0.1.0', 'kind': 'lib', **settings}))
        return package

    @staticmethod
    def dependency(name):
        return {'name': name, 'version': '^0.1.0', 'path': '../' + name}

    def test_imported_macro_reexport_ctfe_and_cache_follow_the_provider(self):
        provider = self.library('provider', macroInputs={'emitValue': {'amount': 'own.txt'}})
        facade = self.library('facade', dependencies={'provider': self.dependency('provider')})
        (provider / 'own.txt').write_text('unused provider input')
        expressions = provider / 'src/tools/.expressions'
        expressions.mkdir(parents=True)
        (expressions / 'emitValue.ly').write_text('''import self.calc
pub fn emitValue(target: String, argument: String): Syntax {
    const Base: Int = calc.offset(4)
    val value = syntaxArgument(0)
    val extra = syntaxInt(Base + macroInput("amount").size())
    quote { { val value = 2; splice(value) + splice(extra) + value } }
}
''')
        calc = provider / 'src/calc'
        calc.mkdir()
        helper = calc / 'math.ly'
        code = '''enum Input { Amount(Int), Empty }
const Extra: Int = 1
fn identity<T>(value: T): T { value }
extension IntOffsets for Int { fn addExtra(): Int { this + Extra } }
pub fn offset(value: Int): Int {
    val values = FrozenList([Input.Amount(identity(value)), Input.Empty])
    var total = 0
    for entry in values { total += match entry { Input.Amount(amount) => amount, Input.Empty => 0 } }
    total.addExtra()
}
'''
        helper.write_text(code)
        (facade / 'src/exports.ly').write_text('pub import macro provider.tools.emitValue as addExtra\n')
        self.manifest(dependencies={'facade': self.dependency('facade')}, macroInputs={'sugar': {'amount': 'amount.txt'}})
        snapshot = self.pkg / 'amount.txt'
        snapshot.write_text('x')
        source = '''import macro facade.addExtra as sugar
fn main(): Unit { val value = 10; print(#sugar(value)) }
'''
        self.native(source, ['18'])
        helper.write_text(code.replace('Extra: Int = 1', 'Extra: Int = 3'))
        self.native(source, ['20'])
        snapshot.write_text('xxxx')
        self.native(source, ['23'])
        self.manifest(dependencies={'facade': self.dependency('facade')})
        self.diagnostic('LYM0002', '未获得只读输入')

    def test_imported_declaration_macro_needs_a_public_entry_and_explicit_exports(self):
        provider = self.library('provider')
        macros = provider / 'src/.macros'
        macros.mkdir()
        implementation = macros / 'makeApi.ly'
        code = '''pub fn makeApi(target: String, argument: String): Syntax {
    val result = syntaxInt(42)
    quote { pub fn api(): Int { splice(result) } }
}
'''
        implementation.write_text(code)
        self.manifest(dependencies={'provider': self.dependency('provider')})
        source = '''import macro provider.makeApi as materialize
#materialize(export api)
fn main(): Unit { print(api()) }
'''
        self.native(source, ['42'])
        (self.src / 'main.ly').write_text(source.replace('(export api)', ''))
        self.diagnostic('LYM0002', '公开声明')
        (self.src / 'main.ly').write_text(source)
        implementation.write_text(code.removeprefix('pub '))
        self.diagnostic('LYM0001', 'pub fn')

    def test_generated_imports_inherit_the_calling_file_without_leaking(self):
        for module, declarations in (
            ('model', 'pub fn value(): Int { 20 }'),
            ('support', 'pub fn value(): Int { 21 }\npub const Offset: Int = 1'),
        ):
            folder = self.src / module
            folder.mkdir()
            (folder / 'api.ly').write_text(declarations)
        self.macro(r'"import self.support as extras\nfn generated(): Int { model.value() + extras.value() + start }"')
        self.native('''import self.model
import self.support.{Offset as start}
#make
fn main(): Unit { print(generated()) }
''', ['42'])
        sibling = self.src / 'sibling.ly'
        sibling.write_text('fn leak(): Int { extras.value() }')
        error = self.diagnostic('LYN0001', 'extras')
        self.assertEqual(Path(error['file']), sibling)

    def test_macro_phase_rejects_generated_and_decorated_dependencies(self):
        provider = self.library('provider')
        expressions = provider / 'src/.expressions'
        expressions.mkdir()
        (expressions / 'current.ly').write_text('''import self.{answer}
pub fn current(target: String, argument: String): Syntax { syntaxInt(answer()) }
''')
        (provider / 'src/.macros').mkdir()
        (provider / 'src/.macros/make.ly').write_text('fn make(target: String, argument: String): Syntax { quote { pub struct Generated(pub val number: Int) } }')
        self.manifest(dependencies={'provider': self.dependency('provider')})
        main = self.src / 'main.ly'
        main.write_text('import macro provider.current\nfn main(): Unit { print(#current()) }')
        functions = provider / 'src/answer.ly'
        generated = '#make(export Generated)\nfn marker(): Unit {}\n'
        functions.write_text(generated + 'pub fn notUsed(value: Generated): Int { print(value.number); 0 }\npub fn answer(): Int { 4 }')
        result = self.cli('run', self.pkg)
        self.assertEqual((result.returncode, result.stdout.splitlines()), (0, [b'4']), result.stderr.decode())
        for body, fragment in (('@trace\npub fn answer(): Int { 4 }', '尚未展开或装饰'),
                               (generated + 'pub fn answer(): Int { Generated(4).number }', 'Generated'),
                               ('pub fn answer(): Int { if false { print(1) }; 4 }', 'print')):
            with self.subTest(body=body):
                functions.write_text(body)
                error = self.diagnostic('LYM0002' if fragment == 'print' else 'LYM0004', fragment)
                self.assertEqual(Path(error['file']), main)
        (provider / 'src/nested').mkdir()
        functions.write_text('pub import macro self.nested.second as current')
        (provider / 'src/nested/api.ly').write_text('pub import macro self.current as second')
        self.diagnostic('LYM0004', '重导出')

    def test_instance_decorators_preserve_receiver_overloads_defaults_and_cold_tasks(self):
        provider = self.library('provider')
        decorators = provider / 'src/.decorators'
        decorators.mkdir()
        (decorators / 'observe.ly').write_text('''pub fn observe(target: String, label: String): Syntax {
    val tag = syntaxString(label)
    val failure = if label == "failure " { quote { throw Exception("hook failure"); } }
                  else { quote { print(exception.message); } }
    syntaxAround(
        quote { print(splice(tag) + this.name); },
        quote { print((result as? Int) ?? 0); },
        failure
    )
}
''')
        self.manifest(dependencies={'provider': self.dependency('provider')})
        self.native('''import decorator provider.observe as traceMember
exception Original
class Box<T>(val name: String, val item: T) {
    @traceMember("outer ")
    @traceMember("inner ")
    async fn read(number: Int = 7): Int {
        if number < 0 { throw Exception("bad") }
        number
    }
    fn read(text: String): String { text + this.name }
    @traceMember("failure ")
    fn fail(): Int { throw Original("original") }
    @trace
    fn identity<U>(value: U) { value }
}
async fn main(): Unit {
    val box = Box("box", 0)
    val pending = box.read()
    print("cold")
    print(await pending)
    print(await pending)
    val bound: (Int) -> Task<Int> = box.read
    try { val _ = await bound(-1) } catch e: Exception { print(e.message) }
    print(box.read("sync "))
    print(box.identity("generic"))
    try { val _ = box.fail() } catch e: Original { print(e.message); print(e.cleanupFailures[0].message) }
}
''', ['cold', 'outer box', 'inner box', '7', '7', '7', '7', 'outer box', 'inner box', 'bad', 'bad', 'bad',
      'sync box', 'trace: identity', 'generic', 'failure box', 'original', 'hook failure'])

    def test_quote_splice_diagnostics_keep_the_argument_range(self):
        expressions = self.src / '.expressions'
        expressions.mkdir()
        (expressions / 'increment.ly').write_text('''fn increment(target: String, argument: String): Syntax {
    quote { requireInt(splice(syntaxArgument(0))) }
}
''')
        main = self.src / 'main.ly'
        source = 'fn requireInt(value: Int): Int { value }\nfn main(): Unit { val label = "值🙂"; print(#increment(label)) }'
        main.write_text(source, encoding='utf-8', newline='\n')
        error = self.diagnostic('LYT0001', '实际是 String')
        self.assertEqual(Path(error['file']), main)
        self.assertEqual(error['range']['start']['offset'], source.encode('utf-8').rfind(b'label'))

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

    def test_expansion_cache_reuses_output_and_invalidates_changed_dependencies(self):
        cache = self.root / 'cache/compiler/macros'
        expressions = self.src / '.expressions'
        expressions.mkdir()
        implementation = expressions / 'value.ly'
        macro = '''fn value(target: String, raw: String): Syntax {
    syntaxCall(syntaxName("combine"), syntaxArgs(syntaxArgument(0), syntaxCode(macroInput("extra") + " + 1")))
}'''
        implementation.write_text(macro)
        main = self.src / 'main.ly'
        source = 'fn main(): Unit { print(#value(7)) }\n'
        main.write_text(source)
        helper = self.src / 'helper.ly'
        helper.write_text('fn combine(first: Int, second: Int): Int { first + second }')
        snapshot = self.pkg / 'extra.txt'
        snapshot.write_text('5')
        self.manifest(macroInputs={'value': {'extra': 'extra.txt'}})

        def run(expected):
            result = self.cli('run', self.pkg)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertEqual(result.stdout.decode().splitlines(), [str(expected)])

        run(13)
        entry, = cache.glob('*.expansion')
        contents = entry.read_bytes()
        old_time = 1_000_000_000
        os.utime(entry, ns=(old_time, old_time))
        run(13)
        self.assertEqual(list(cache.iterdir()), [entry])
        self.assertEqual(entry.stat().st_mtime_ns, old_time)

        helper.write_text('fn combine(first: Bool, second: Int): Int { second }')
        error = self.diagnostic('LYT0001')
        self.assertEqual((Path(error['file']), error['range']['start']['offset']), (main, source.index('7')))
        self.assertEqual(entry.stat().st_mtime_ns, old_time)
        helper.write_text('fn combine(first: Int, second: Int): Int { first + second }')

        entry.write_bytes(b'incomplete expansion')
        run(13)
        self.assertEqual(entry.read_bytes(), contents)
        snapshot.write_text('6')
        run(14)
        main.write_text(source.replace('7', '8'))
        run(15)
        implementation.write_text(macro.replace(' + 1', ' + 2'))
        run(16)
        self.assertEqual(len(list(cache.glob('*.expansion'))), 4)

        snapshot.unlink()
        self.diagnostic('LYM0002', '不存在')
        self.assertEqual(len(list(cache.glob('*.expansion'))), 4)

    def test_cached_field_defaults_are_resolved_again_for_the_current_module(self):
        main = self.src / 'main.ly'
        main.write_text('''#derive(Json)
struct Item(pub val count: Int = 41 + 1)
fn main(): Unit {
    print(match decodeItem("{}") { Ok(item) => item.count, Err(_) => -1 })
}
''')
        result = self.cli('run', self.pkg)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(result.stdout.decode().splitlines(), ['42'])
        entry, = (self.root / 'cache/compiler/macros').glob('*.expansion')
        old_time = 1_000_000_000
        os.utime(entry, ns=(old_time, old_time))
        (self.src / 'early.ly').write_text('fn early(): Bool { true }')
        result = self.cli('run', self.pkg)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(result.stdout.decode().splitlines(), ['42'])
        self.assertEqual(entry.stat().st_mtime_ns, old_time)

    def test_unavailable_cache_leaves_macro_behavior_unchanged(self):
        (self.src / 'main.ly').write_text('#make\nfn main(): Unit { print(generated()) }')
        self.macro('"fn generated(): Int { 42 }"')
        self.manifest(macroInputs={'make': {'unused': 'missing.txt'}})
        result = self.cli('check', self.pkg)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertFalse((self.root / 'cache/compiler/macros').exists())

        self.manifest()
        (self.root / 'cache').mkdir(exist_ok=True)
        (self.root / 'cache/compiler').write_text('not a directory')
        result = self.cli('check', self.pkg)
        self.assertEqual(result.returncode, 0, result.stderr.decode())

    def test_class_decorator_generates_authorized_api(self):
        decorators = self.src / '.decorators'
        decorators.mkdir()
        (decorators / 'describe.ly').write_text('''
fn describe(target: String, argument: String): Syntax {
    syntaxCode("pub fn generatedTypeName(): String { " + macroQuote(target) + " }")
}
''')
        self.native('''
@describe(export generatedTypeName)
class Record(val value: Int)
fn main(): Unit { print(generatedTypeName() + ":" + Record(7).value.toString()) }
''', ['Record:7'])

        (self.src / 'main.ly').write_text('''
@describe
class Record(val value: Int)
fn main(): Unit {}
''')
        self.diagnostic('LYM0002', '公开')

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
        (self.src / '.macros/make.ly').write_text('extern fn forbidden(): Int;\nfn make(target: String, argument: String): Syntax { syntaxCode("") }')
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

    def test_json_defaults_share_constructor_values_and_keep_explicit_null(self):
        self.native(r'''class Memo(val text: String)
fn intText(value: Int): String { print("default name"); "guest" }
fn remember(name: String?): Memo { val text = name ?? "anonymous"; print("memo " + text); Memo(text) }
#derive(Json)
struct User(pub val name: String? = intText(0), val memo: Memo = remember(name),
            [jsonName("display-name")] pub val label: String = memo.text, pub val note: String? = null)
fn show(text: String): String { match decodeUser(text) { Ok(user) => encodeUser(user), Err(error) => error } }
fn main(): Unit {
    print(encodeUser(User()))
    print(show("{}"))
    print(show(r#"{"name":null}"#))
    print(show(r#"{"name":"Ada","display-name":"A","note":"hello"}"#))
    print(show(r#"{"display-name":false}"#))
    print(show(r#"{"display-name":null}"#))
    print(show(r#"{"memo":"fake"}"#))
    print(match decodeUserIgnoringUnknownFields(r#"{"name":"Bo","memo":"fake"}"#) {
        Ok(user) => user.memo.text, Err(error) => error,
    })
}''', ['default name', 'memo guest', '{"name":"guest","display-name":"guest"}',
       'default name', 'memo guest', '{"name":"guest","display-name":"guest"}',
       'memo anonymous', '{"name":null,"display-name":"anonymous"}',
       'memo Ada', '{"name":"Ada","display-name":"A","note":"hello"}',
       '$["display-name"]: expected String', '$["display-name"]: expected String',
       '$.memo: unknown field', 'memo Bo', 'Bo'])

    def test_json_nested_optional_with_literal_none_keeps_three_states(self):
        self.native(r'''#derive(Json)
struct Change(pub val note: Option<String?> = None)
fn show(text: String): String { match decodeChange(text) { Ok(change) => encodeChange(change), Err(error) => error } }
fn main(): Unit {
    print(show("{}")); print(show(r#"{"note":null}"#)); print(show(r#"{"note":"Ada"}"#))
}''', ['{}', '{"note":null}', '{"note":"Ada"}'])
        main = self.src / 'main.ly'
        for field, reason in (('pub val note: Option<String?> = Some(None)', 'custom JSON mapping'),
                              ('val note: String?', 'private field without a default')):
            with self.subTest(field=field):
                main.write_text('#derive(Json)\nstruct Bad(' + field + ')\nfn main(): Unit {}')
                self.diagnostic('LYM0002', reason)

    def test_derive_is_opt_in_for_every_type(self):
        main = self.src / 'main.ly'
        main.write_text('struct Item(pub val id: Int)\nfn main(): Unit { print(encodeItem(Item(7))) }')
        self.diagnostic('LYN0001', 'encodeItem')
        main.write_text('struct Item(pub val id: Int)\n#derive(Json)\nstruct Envelope(pub val item: Item)\nfn main(): Unit {}')
        error = self.diagnostic('LYN0001', 'encodeItem')
        self.assertEqual(error['range']['start']['line'], 3)
        self.assertIn('item', error['generated']['source'])

    def test_resolved_declaration_types_follow_aliases_and_invalidate_cache(self):
        (self.src / '.macros/inspect.ly').write_text('''fn inspect(target: String, argument: String): Syntax {
    val first = syntaxResolvedType(syntaxChild(syntaxChild(syntaxTarget(), 1), 1))
    val element = syntaxChild(first, 0)
    val leaf = syntaxChild(element, 0)
    val generic = syntaxResolvedType(syntaxChild(syntaxChild(syntaxTarget(), 2), 1))
    val local = syntaxResolvedType(syntaxChild(syntaxChild(syntaxTarget(), 3), 1))
    val text = syntaxText(first) + "/" + syntaxText(element) + "/" + syntaxText(leaf) + "/" + syntaxKind(generic) + ":" + syntaxText(generic) + "/" + syntaxTypeModule(local) + ":" + syntaxText(local)
    syntaxCode("fn shape(): String { " + macroQuote(text) + " }")
}''', encoding='utf-8')
        model = self.src / 'model'
        model.mkdir()
        aliases = model / 'model.ly'
        api = self.src / 'api'
        api.mkdir()
        (api / 'api.ly').write_text('pub import self.model.{Payload as Wire}\npub import self.model.{Named}', encoding='utf-8')
        main = self.src / 'main.ly'
        main.write_text('''import self.api.{Wire as Alias}
import self.api.{Named as PublicName}
#inspect
struct Probe<T>(val items: Alias, val other: T, val record: PublicName)
fn main(): Unit { print(shape()) }
''', encoding='utf-8')
        for kind in ('Int8', 'String'):
            aliases.write_text('struct Private()\npub struct Named()\ntype Leaf = ' + kind + '\npub type Payload = List<Leaf?>', encoding='utf-8')
            run = self.cli('run', self.pkg)
            self.assertEqual(run.returncode, 0, run.stderr.decode())
            text = run.stdout.decode().strip()
            self.assertTrue(text.startswith(f'List/Option/{kind}/parameter:T/'), text)
            self.assertTrue(text.endswith('.model:Named'), text)
        self.assertEqual(len(list((self.root / 'cache/compiler/macros').glob('*.expansion'))), 2)

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
        self.native(r'''import std.json.{parseJson}
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

    def test_json_char_is_exactly_one_unicode_scalar(self):
        expected_char = ': expected Char (a string containing one Unicode scalar)'
        self.native(r'''#derive(Json)
struct Marks([jsonName("display-mark")] pub val symbol: Char, pub val rows: List<List<Char?>>)
fn show(text: String): String { match decodeMarks(text) { Ok(value) => encodeMarks(value), Err(error) => error } }
fn main(): Unit {
    val marks = Marks('🙂', [[Some('中'), None], [Some('\0'), Some('"')]])
    print(show(encodeMarks(marks)))
    print(show(r#"{"display-mark":"\ud83d\ude42","rows":[["a","é","\\"]]}"#))
    print(show(r#"{"display-mark":"","rows":[]}"#))
    print(show(r#"{"display-mark":"ab","rows":[]}"#))
    print(show(r#"{"display-mark":101,"rows":[]}"#))
    print(show(r#"{"rows":[]}"#))
    print(show(r#"{"display-mark":"é","rows":[[],["e\u0301"]]}"#))
    print(match decodeMarks(r#"{"display-mark":"\ud800","rows":[]}"#) { Ok(_) => "accepted", Err(_) => "invalid Unicode rejected" })
}''', [json.dumps({'display-mark': '🙂', 'rows': [['中', None], ['\0', '"']]}, ensure_ascii=False, separators=(',', ':')),
       json.dumps({'display-mark': '🙂', 'rows': [['a', 'é', '\\']]}, ensure_ascii=False, separators=(',', ':')),
       *(['$["display-mark"]' + expected_char] * 4), '$.rows[1][0]' + expected_char, 'invalid Unicode rejected'])

    def test_json_ignores_unknown_fields_only_on_the_selected_view(self):
        self.native(r'''import std.json.{JsonValue}
import std.json.{parseJson}
#derive(Json)
struct User([jsonName("user-id")] pub val id: Int)
struct Tagged(pub val user: User)
fn encodeTagged(value: Tagged): String { encodeUser(value.user) }
fn readTaggedJson(value: JsonValue, path: String): Result<Tagged,String> { Ok(Tagged(readUserJson(value, path)?)) }
#derive(Json)
struct Batch(pub val items: List<Tagged?>?)
fn show(text: String, decode: (String) -> Result<Batch,String>): String {
    match decode(text) { Ok(batch) => encodeBatch(batch), Err(error) => error }
}
fn readBoth(text: String): Result<String,String> {
    val root = parseJson(text)?
    val batch = readBatchJson(root.ignoringUnknownFields(), "$")?
    match readBatchJson(root, "$") { Ok(_) => Ok("unexpected strict success"), Err(error) => Ok(encodeBatch(batch) + " / " + error) }
}
fn main(): Unit {
    val top = r#"{"items":[null,{"user-id":7,"future.key":[true]}],"extra":{"name":42}}"#
    val nested = r#"{"items":[null,{"user-id":7,"future.key":[true]}]}"#
    print(show(top, decodeBatch)); print(show(top, decodeBatchIgnoringUnknownFields))
    print(match readBoth(nested) { Ok(message) => message, Err(error) => error })
    print(show(r#"{"items":[{"id":7}]}"#, decodeBatchIgnoringUnknownFields))
    print(show(r#"{"items":[{"extra":1,"user-id":false}]}"#, decodeBatchIgnoringUnknownFields))
    print(show(r#"{"items":{}}"#, decodeBatchIgnoringUnknownFields))
    print(match decodeBatchIgnoringUnknownFields(r#"{"ignored":{"duplicate":1,"duplicate":2}}"#) { Ok(_) => "accepted", Err(_) => "duplicate rejected" })
}''', ['$.extra: unknown field', '{"items":[null,{"user-id":7}]}',
       '{"items":[null,{"user-id":7}]} / $.items[1]["future.key"]: unknown field',
       '$.items[0]["user-id"]: expected Int', '$.items[0]["user-id"]: expected Int',
       '$.items: expected array', 'duplicate rejected'])

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
        (self.src / '.macros/post.ly').write_text('''fn post(target: String, route: String): Syntax {
    if !fieldHasAttribute(0, "path") || fieldType(0) != "Int" { return syntaxError(syntaxTarget(), "expected Int path") }
    val key = macroQuote(fieldAttributeValue(1, "query"))
    syntaxCode("fn adapt(request: Request): String { " + target + "(request." + fieldName(0) + ", request.query(" + key + ")) }\\nfn route(): String { " + macroQuote(route) + " }")
}''')
        self.native('''struct Request(val id: Int) { fn query(key: String): String { key + "!" } }
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
        expression.write_text('''fn make(target: String, raw: String): Syntax {
    if argumentCount() != 2 { return syntaxError(syntaxCode(""), "expected two expressions") }
    syntaxConcat(syntaxCode("{ val temp = 10; if "),
        syntaxConcat(syntaxArgument(0), syntaxConcat(syntaxCode(" { temp + "),
            syntaxConcat(syntaxArgument(1), syntaxCode(" } else { 0 } }")))))
}''')
        self.native('''fn add(left: Int, right: Int): Int { print("add"); left + right }
fn first<A,B>(first: A, second: B): A { first }
struct Pair<First,Second> {
    static fn first(value: First, second: Second): First { value }
}
fn main(): Unit {
    val temp = 2
    print(2 * #make(true, add(temp, 1)))
    print(#make(false, add(temp, 9)))
    print(#make(true, #make(true, temp)))
    print(#make(temp < 3, first<Int,List<Int>>(4, [1, 2])))
    print(#make(true, Pair<Int,List<Int>>.first(7, [3])))
}''', ['add', '26', '0', '22', '14', '17'])
        main = self.src / 'main.ly'
        main.write_text('fn main(): Unit {\n    print(#make(true, false))\n}')
        error = self.diagnostic('LYT0001')
        self.assertEqual((error['range']['start']['line'], error['range']['start']['column']), (2, 23))
        expression.write_text('fn make(target: String, raw: String): Syntax { syntaxName("temp") }')
        main.write_text('fn main(): Unit { val temp = 7; print(#make()) }')
        error = self.diagnostic('LYN0001', 'temp')
        self.assertEqual(error['range']['start']['column'], main.read_text().index('#make') + 2)
        self.assertIn('temp', error['generated']['source'])

    def test_expression_macro_uses_only_declared_snapshot_and_one_expression(self):
        directory = self.src / '.expressions'
        directory.mkdir()
        impl = directory / 'query.ly'
        impl.write_text('''fn query(target: String, raw: String): Syntax {
    if argumentString(0) != macroInput("statement") { return syntaxError(syntaxArgument(0), "refresh query snapshot") }
    syntaxCall(syntaxName("bindId"), syntaxArgument(1))
}''')
        self.manifest(macroInputs={'query': {'statement': 'inputs/query.sql'}})
        (self.pkg / 'inputs').mkdir()
        (self.pkg / 'inputs/query.sql').write_text('SELECT ?1')
        code = 'fn bindId(id: Int): Int { id }\nfn main(): Unit { print(#query("SELECT ?1", 7)) }'
        self.native(code, ['7'])
        self.assertNotEqual(self.cli('build', self.pkg, '-o', impl).returncode, 0)
        main = self.src / 'main.ly'
        main.write_text(code.replace('SELECT ?1', 'SELECT ?2'))
        error = self.diagnostic('LYM0002', 'snapshot')
        self.assertEqual(error['range']['start']['offset'], main.read_bytes().index(b'"SELECT ?2"'))
        main.write_text(code.replace(', 7)', ', "bad")'))
        error = self.diagnostic('LYT0001')
        self.assertEqual(error['range']['start']['offset'], main.read_bytes().index(b'"bad"'))
        snapshot = self.pkg / 'inputs/query.sql'
        snapshot.unlink()
        main.write_text(code)
        self.diagnostic('LYM0002', '不存在')
        snapshot.write_text('SELECT ?1')
        main.write_text(code.replace('"SELECT ?1"', '7'))
        self.diagnostic('LYM0002', '字面量')
        main.write_text(code)
        for generated in ('1); pub fn escaped(): Int { 9 }', '#query(7)'):
            impl.write_text('fn query(target: String, raw: String): Syntax { syntaxCode(' + json.dumps(generated) + ') }')
            self.diagnostic('LYM0002')
        main.write_text('fn main(): Unit { print(#derive(Json)) }')
        self.diagnostic('LYM0001', '表达式宏')

    def test_declaration_nodes_keep_positions_and_only_requested_names_are_public(self):
        macro = self.src / '.macros/checkBody.ly'
        macro.write_text('''fn checkBody(target: String, argument: String): Syntax {
    val body = syntaxBody()
    val statement = syntaxChild(body, 0)
    if syntaxKind(statement) != "return" { return syntaxError(statement, "first statement must return") }
    if syntaxLine(statement) < 3 || syntaxStart(statement) >= syntaxEnd(statement) { return syntaxError(statement, "missing span") }
    syntaxCode("pub fn generated(): Int { 7 }")
}''')
        self.native('''#checkBody(export generated)
fn target(): Int {
    return 42
}
fn main(): Unit { print(generated()) }
''', ['7'])
        main = self.src / 'main.ly'
        main.write_text('#checkBody(export generated)\nfn target(): Int { 42 }\nfn main(): Unit {}\n')
        error = self.diagnostic('LYM0002', 'first statement')
        self.assertEqual(error['range']['start']['offset'], main.read_bytes().index(b'42'))
        main.write_text('#checkBody\nfn target(): Int {\n    return 42\n}\nfn main(): Unit {}\n')
        self.diagnostic('LYM0002', '公开')

    def test_typed_macro_observes_inference_after_expansion_and_invalidates_cache(self):
        directory = self.src / '.typedMacros'
        directory.mkdir()
        (directory / 'describe.ly').write_text('''fn describe(target: String, argument: String): Syntax {
    val body = syntaxBody()
    val binding = syntaxChild(body, 0)
    val name = syntaxChild(binding, 0)
    val initializer = syntaxChild(binding, 1)
    val valueType = match syntaxTypeOf(initializer) { Some(node) => node, None => return syntaxError(initializer, "missing expression type") }
    val localType = match syntaxTypeOf(name) { Some(node) => node, None => return syntaxError(name, "missing binding type") }
    if syntaxTypeOf(syntaxString("generated")) != None { return syntaxError(name, "generated syntax should have no type") }
    val text = target + ":" + syntaxText(syntaxResultType()) + ":" + syntaxText(valueType) + ":" + syntaxText(localType)
    syntaxCode("pub fn observed(): String { " + macroQuote(text) + " }")
}''')
        self.macro('"fn generated() { inferredDependency() }"')
        dependency = self.src / 'dependency.ly'
        dependency.write_text('fn inferredDependency() { 7 }\n')
        source = '''#make
struct Anchor()
#describe(export observed)
@trace
fn target() { val picked = generated(); picked }
fn main(): Unit { print(observed()) }
'''
        self.native(source, ['target:Int:Int:Int'])
        dependency.write_text('fn inferredDependency() { "text" }\n')
        self.native(source, ['target:String:String:String'])

    def test_imported_type_macro_preserves_generic_method_types_and_allows_later_consumers(self):
        provider = self.library('provider')
        directory = provider / 'src/.typedMacros'
        directory.mkdir()
        (directory / 'inspect.ly').write_text('''pub fn inspect(target: String, argument: String): Syntax {
    val result = syntaxResultType()
    val field = syntaxChild(syntaxChild(syntaxTarget(), 1), 1)
    val param = match syntaxTypeOf(field) { Some(value) => value, None => return syntaxError(field, "missing signature type") }
    val description = syntaxKind(result) + ":" + syntaxText(result) + ":" + syntaxText(param)
    syntaxCode("pub struct Report(pub val text: String = " + macroQuote(description) + ")")
}''')
        facade = self.library('facade', dependencies={'provider': self.dependency('provider')})
        (facade / 'src/export.ly').write_text('pub import macro provider.inspect as inspectMethod\n')
        self.manifest(dependencies={'facade': self.dependency('facade')})
        module = self.src / 'widgets'
        module.mkdir()
        (module / 'widget.ly').write_text('''import macro facade.inspectMethod
struct Widget() {
    #inspectMethod(export Report)
    fn select<Item>(value: Item) { val local = value; local }
}
''')
        self.native('''import self.widgets.{Report}
fn describe(report: Report): String { report.text }
fn main(): Unit { print(describe(Report())) }
''', ['parameter:Item:Item'])

    def test_type_macro_phase_cycle_points_to_the_dependency(self):
        directory = self.src / '.typedMacros'
        directory.mkdir()
        code = 'fn makeLater(target: String, argument: String): Syntax { syntaxCode("pub fn later(): Int { 5 }") }'
        (directory / 'makeLater.ly').write_text(code)
        main = self.src / 'main.ly'
        main.write_text('''#makeLater(export later)
fn target() { dependency() }
fn dependency() { later() }
fn main(): Unit {}
''')
        error = self.diagnostic('LYM0004', '阶段环')
        self.assertEqual(error['range']['start']['offset'], main.read_bytes().index(b'later()'))
        (self.src / '.macros/makeLater.ly').write_text(code)
        self.diagnostic('LYM0001', '同名')

    def test_type_macro_cannot_add_an_overload_that_changes_the_observed_target(self):
        directory = self.src / '.typedMacros'
        directory.mkdir()
        (directory / 'addOverload.ly').write_text('''fn addOverload(target: String, argument: String): Syntax {
    quote { fn choice(value: Int): String { "changed" } }
}''')
        (self.src / 'main.ly').write_text('''fn choice(value: Any): Int { 7 }
#addOverload
fn target() { choice(1) }
fn main(): Unit {}
''')
        self.diagnostic('LYM0002', '不能添加现有函数的重载')

    def test_decorator_composes_success_and_failure_without_eager_async_execution(self):
        decorators = self.src / '.decorators'
        decorators.mkdir()
        (decorators / 'observe.ly').write_text('''fn observe(target: String, label: String): Syntax {
    syntaxAround(
        syntaxCall(syntaxName("print"), syntaxString(label + ":before")),
        syntaxCode("if result is Int { print(" + macroQuote(label + ":ok") + "); print(result as? Int ?? 0) }"),
        syntaxCode("print(" + macroQuote(label + ":error") + "); print(exception.message)"))
}''')
        self.native('''exception Oops
@observe("outer")
@observe("inner")
async fn work(fail: Bool): Int { if fail { throw Oops("failed") }; 42 }
async fn main(): Unit {
    val callable: (Bool) -> Task<Int> = work
    val cold = callable(false)
    print("cold")
    print(await cold)
    print(await cold)
    try { await work(true); } catch error: Oops { print(error.message) }
}
''', ['cold', 'outer:before', 'inner:before', 'inner:ok', '42', 'outer:ok', '42', '42', '42',
      'outer:before', 'inner:before', 'inner:error', 'failed', 'outer:error', 'failed', 'failed'])


if __name__ == '__main__':
    unittest.main()
