import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class Modules(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.app = self.root / 'app'
        self.lib = self.root / 'lib'
        self.env = {**os.environ, 'LYRA_CACHE': str(self.root / 'cache')}
        (self.app / 'src').mkdir(parents=True)
        (self.lib / 'src/net').mkdir(parents=True)
        (self.lib / 'lyra.json5').write_text(json.dumps(dict(name='lib', version='0.1.0', kind='lib')))
        manifest = dict(name='app', version='0.1.0', dependencies={
            'cfg': dict(name='lib', version='^0.1.0', path='../lib')})
        (self.app / 'lyra.json5').write_text(json.dumps(manifest))
        (self.lib / 'src/net/defaults.ly').write_text('fn hidden(): Int { 20 }\npub fn port(): Int { hidden() + 1 }\n')
        (self.lib / 'src/lib.ly').write_text('pub import self.net.{port as defaultPort}\n')
        self.source = self.app / 'src/main.ly'
        self.source.write_text('import cfg.{defaultPort as port}\nfn hidden(): Int { 1 }\nfn main(): Unit { print(port() * 2) }\n')

    def command(self, *args, ok=True):
        result = subprocess.run([COMPILER, *args], capture_output=True, text=True, timeout=25, env=self.env)
        self.assertEqual(result.returncode == 0, ok, result.stderr)
        return result

    def test_run_test_and_locked_rebuild(self):
        self.assertEqual(self.command('run', self.app).stdout, '42\n')
        shutil.rmtree(self.app / 'build')
        self.assertEqual(self.command('run', self.app, '--locked', '--offline', '--release').stdout, '42\n')
        self.command('check', self.lib)
        self.command('build', self.lib, ok=False)
        self.assertIn('LYC0003', self.command('test', self.lib, ok=False).stderr)
        (self.lib / 'tests').mkdir()
        (self.lib / 'tests/port.ly').write_text('import self.{defaultPort}\nfn main(): Unit { print(defaultPort()) }')
        self.assertIn('21\nok lib/port.ly', self.command('test', self.lib).stdout)

    def test_private_import_alias_and_cycles(self):
        cases = [('import cfg.net.{hidden}', 'LYN0003'), ('import cfg.net.{missing}', 'LYN0003'),
                 ('import other.net.{port}', 'LYN0003'), ('import cfg.net.{port as main}', 'LYN0002')]
        for declaration, code in cases:
            self.source.write_text(declaration + '\nfn main(): Unit {}')
            diagnostic, = json.loads(self.command('check', self.app, '--diagnostics=json', ok=False).stderr)
            self.assertEqual(diagnostic['code'], code)
            self.assertEqual(diagnostic['range']['start']['line'], 1)
        (self.lib / 'src/lib.ly').write_text('pub import self.net.{loop as defaultPort}')
        (self.lib / 'src/net/alias.ly').write_text('pub import self.{defaultPort as loop}')
        self.source.write_text('import cfg.{defaultPort}\nfn main(): Unit {}')
        diagnostic, = json.loads(self.command('check', self.app, '--diagnostics=json', ok=False).stderr)
        self.assertEqual(diagnostic['code'], 'LYN0004')

    def test_file_local_namespaces_and_shared_reexports(self):
        (self.app / 'src/net').mkdir()
        (self.app / 'src/net/defaults.ly').write_text('pub fn port(): Int { 7 }\n')
        (self.app / 'src/sibling.ly').write_text('''import self.net
import self.net.{port as siblingPort}
pub import cfg.net.{port as sharedPort}
fn localPort(): Int { net.port() + siblingPort() }
''')
        self.source.write_text('''import cfg.net
import self.net as localNet
import cfg.{
    defaultPort as port,
}
fn main(): Unit {
    print(net.port() + localNet.port() + localPort() + sharedPort() + port())
    val net = [3, 4]
    print(net.size)
}
''')
        self.assertEqual(self.command('run', self.app).stdout, '84\n2\n')

        for imports, body, code, detail in (
            ('import cfg.net\nimport self.net', '', 'LYN0002', 'net'),
            ('import cfg.net', 'port();', 'LYN0001', 'port'),
            ('', 'net.port();', 'LYN0001', 'net'),
            ('', 'siblingPort();', 'LYN0001', 'siblingPort'),
            ('import cfg.net', 'net.hidden();', 'LYN0003', 'net.hidden'),
            ('import cfg.net', 'val unused = net', 'LYN0003', 'net'),
            ('import cfg.net.port', '', 'LYN0003', '.{port}'),
            ('pub import cfg.net', '', 'LYP0001', '.{'),
            ('import cfg.net.{*}', '', 'LYP0001', None),
        ):
            with self.subTest(imports=imports, body=body):
                self.source.write_text(imports + '\nfn main(): Unit { ' + body + ' }')
                diagnostic, = json.loads(self.command('check', self.app, '--diagnostics=json', ok=False).stderr)
                self.assertEqual(diagnostic['code'], code, diagnostic)
                if detail:
                    self.assertIn(detail, diagnostic['message'])

    def test_qualified_generic_types_patterns_and_overloads(self):
        (self.lib / 'src/net/types.ly').write_text('''pub trait Readable { type Item; fn get(): Item }
pub struct Box<T>(pub val value: T) {
    pub type Item = T
    pub fn get(): T { value }
    pub static fn create(value: T): Box<T> { Box(value) }
}
pub enum State<T> { Missing, Ready(Box<T>) }
pub const Limit: Int = 42
pub fn identity<T>(value: T): T { value }
pub fn choose(value: Int): String { "int" }
pub fn choose(value: Bool): String { "bool" }
fn choose(value: String): String { "private" }
pub extension BoxOps<T> for Box<T> { fn unwrap(): T { this.value } }
''')
        self.source.write_text('''import cfg.net
import cfg.net.{BoxOps}
fn read<T: net.Readable<Item=Int>>(box: T): T.Item { box.get() }
fn use(box: dyn net.Readable<Item=Int>): Int { box.get() }
fn main(): Unit {
    val box: net.Box<Int> = net.Box<Int>.create(net.identity<Int>(net.Limit))
    val state: net.State<Int> = net.State<Int>.Ready(box)
    print(match state { net.State.Ready(net.Box(value: number)) => number, net.State.Missing => 0 })
    print(match box.get() { net.Limit => "limit", _ => "other" })
    print(read(box))
    print(use(box as dyn net.Readable<Item=Int>))
    print(box.unwrap())
    val choose: (Int) -> String = net.choose
    print(choose(7))
    print(net.choose(true))
}
''')
        self.assertEqual(self.command('run', self.app).stdout, '42\nlimit\n42\n42\n42\nint\nbool\n')

        for body, code in (
            ('net.choose("hidden");', 'LYT0019'),
            ('net.Box(7).unwrap();', 'LYN0001'),
        ):
            with self.subTest(body=body):
                self.source.write_text('import cfg.net\nfn main(): Unit { ' + body + ' }')
                diagnostic, = json.loads(self.command('check', self.app, '--diagnostics=json', ok=False).stderr)
                self.assertEqual(diagnostic['code'], code, diagnostic)
                self.assertNotIn('choose(String)', diagnostic['message'])

    def test_workspace_tests_use_member_not_sibling_entry(self):
        (self.root / 'lyra.json5').write_text('{workspace: {members: ["app", "lib"]}}')
        self.command('check', self.root)
        self.assertEqual(self.command('run', self.app, '--locked').stdout, '42\n')
        self.command('run', self.root, ok=False)

    def test_struct_patterns_resolve_import_alias_and_check_field_visibility(self):
        (self.lib / 'src/net/box.ly').write_text('pub struct Box(pub val number: Int, val secret: Int)\n'
                                                 'pub fn makeBox(): Box { Box(42, 7) }\n')
        imports = 'import cfg.net.{Box as Item}\nimport cfg.net.{makeBox}\n'
        self.source.write_text(imports + 'fn main(): Unit { print(match makeBox() { Item(number: number) => number }) }\n')
        self.assertEqual(self.command('run', self.app).stdout, '42\n')
        self.source.write_text(imports + 'fn main(): Unit { match makeBox() { Item(secret: _) => {} } }\n')
        error, = json.loads(self.command('check', self.app, '--diagnostics=json', ok=False).stderr)
        self.assertEqual(error['code'], 'LYN0003')
        self.assertIn('secret', error['message'])

    def test_type_and_constant_aliases_through_reexports(self):
        (self.lib / 'src/net/types.ly').write_text('pub type Identity = Int\npub const Limit: Identity = 42\n', encoding='utf-8')
        (self.lib / 'src/lib.ly').write_text('pub import self.net.{Identity as Owner}\npub import self.net.{Limit as Capacity}\n', encoding='utf-8')
        self.source.write_text('''import cfg.{Owner as UserId}
import cfg.{Capacity as Limit}
type LocalId = UserId
fn classify(value: LocalId): String {
    match value { Limit => "full", `Limit` => "free ${Limit}" }
}
fn main(): Unit {
    val owner: UserId = 42
    print(classify(owner))
    print(classify(7))
}
''', encoding='utf-8')
        self.assertEqual(self.command('run', self.app).stdout, 'full\nfree 7\n')

    def test_generic_aliases_through_reexports_and_inference(self):
        (self.lib / 'src/net/types.ly').write_text('''
pub struct Entry<T>(pub val value: T)
pub type Page<T> = List<Entry<T>>
pub type Visit<T> = (T) -> String
pub fn first<T>(page: Page<T>): T { page[0].value }
pub fn visit<T>(item: T, format: Visit<T>): String { format(item) }
''')
        (self.lib / 'src/lib.ly').write_text('pub import self.net.{Page as Entries}\n')
        self.source.write_text('''
import cfg.{Entries as Page}
import cfg.net.{Entry}
import cfg.net.{first}
import cfg.net.{visit}
type Id<T> = T
fn forward<T>(items: Page<T>): T { first(items) }
fn main(): Unit {
    val page: Id<Id<Page<Int>>> = [Entry(42)]
    val boxed: Any = page
    print(boxed is List<Entry<Int>>)
    print(visit(forward(page)) { it.toString() })
}
''')
        self.assertEqual(self.command('run', self.app).stdout, 'true\n42\n')

    def test_enum_variants_and_implicit_conversion_across_modules(self):
        (self.lib / 'src/net/status.ly').write_text('''
enum Internal { Wait }
pub enum Status { Wait, Ready(Int) }
pub implicit (value: Status): Int { match value { Wait => 0, Ready(port) => port } }
''')
        self.source.write_text('''
import cfg.net.{Status as Endpoint}
fn identity<T>(value: T): T { value }
fn show(value: Int): Unit { print(value) }
fn main(): Unit {
    show(identity(Endpoint.Ready(42)))
    print(match identity(Endpoint.Wait) { Endpoint.Wait => 1, Ready(_) => 0 })
}
''')
        self.assertEqual(self.command('run', self.app, '--release').stdout, '42\n1\n')
        self.source.write_text('import cfg.net.{Internal}\nfn main(): Unit {}')
        error, = json.loads(self.command('check', self.app, '--diagnostics=json', ok=False).stderr)
        self.assertEqual(error['code'], 'LYN0003')

    def test_generic_record_import_keeps_one_nominal_type(self):
        (self.lib / 'src/net/status.ly').write_text('''
pub struct Status<Item>(pub val item: Item) {
    pub fn read(): Item { item }
    pub static fn make(value: Item): Status<Item> { Status(value) }
    pub static fn from<Input>(value: Input, transform: (Input) -> Item): Status<Item> {
        Status<Item>.make(transform(value))
    }
    static fn hidden(): Int { 7 }
}
pub fn wrap<Item>(value: Item): Status<Item> { Status(value) }
''')
        self.source.write_text('''
import cfg.net.{Status as Endpoint}
import cfg.net.{wrap}
fn show<Item>(value: Endpoint<Item>): Item { value.read() }
fn main(): Unit {
    print(show(wrap(42)))
    print(match Endpoint.make("network") { Endpoint(item: message) => message })
    print(Endpoint<Int>.from<String>("abc") { it.size }.read())
    val inferred: Endpoint<String> = Endpoint.from(7) { it.toString() }
    print(inferred.read())
}
''')
        self.assertEqual(self.command('run', self.app, '--release').stdout, '42\nnetwork\n3\n7\n')
        self.source.write_text('import cfg.net.{Status as Endpoint}\nfn main(): Unit { Endpoint<Int>.hidden(); }')
        diagnostic, = json.loads(self.command('check', self.app, '--diagnostics=json', ok=False).stderr)
        self.assertEqual(diagnostic['code'], 'LYN0003')

    def test_generic_enum_reexport_construct_and_match(self):
        (self.lib / 'src/net/load.ly').write_text('''
pub struct Entry<Item>(pub val value: Item)
pub enum Load<Item> { Pending, Ready(entry: Entry<Item>, next: Load<Item>), Failed(String) }
pub fn ready<Item>(item: Item): Load<Item> { Load.Ready(Entry(item), Load<Item>.Pending) }
''')
        (self.lib / 'src/lib.ly').write_text('pub import self.net.{Load as LoadState}\n')
        imports = '''import cfg.{LoadState as State}
import cfg.net.{Entry}
import cfg.net.{ready}
'''
        self.source.write_text(imports + '''fn head<Item>(state: State<Item>): Item? {
    match state { State.Pending => None, Ready(entry: Entry(value: value), next: _) => Some(value), Failed(_) => None }
}
fn main(): Unit {
    val pending: State<String> = State.Pending
    val inferred = State.Ready(Entry(41), State.Pending)
    val explicit = State<Int>.Ready(Entry(42), State<Int>.Pending)
    print(head(inferred) ?? 0)
    print(head(explicit) ?? 0)
    print(head(ready("ok")) ?? "missing")
    print(head(pending) ?? "pending")
}
''')
        for options in ((), ('--release',)):
            self.assertEqual(self.command('run', self.app, *options).stdout, '41\n42\nok\npending\n')
        for body, code, detail in (
            ('fn main(): Unit { val unresolved = State.Pending }', 'LYG0002', 'Item'),
            ('fn read(state: State<Int>): Int { match state { Pending => 0, Ready(_, _) => 1 } }\nfn main(): Unit {}', 'LYT0012', 'Failed'),
            ('fn main(): Unit { val invalid: State<Bool> = ready(42) }', 'LYT0001', 'Load<Int>'),
        ):
            self.source.write_text(imports + body)
            error, = json.loads(self.command('check', self.app, '--diagnostics=json', ok=False).stderr)
            self.assertEqual(error['code'], code)
            self.assertIn(detail, error['message'])

    def test_extension_group_visibility_priority_and_ambiguity(self):
        (self.lib / 'src/net/extensions.ly').write_text('''
pub class Box(val secret: Int) {
    pub fn choose(value: Int?): String { "member" }
    fn display(): String { "private" }
}
pub fn box(): Box { Box(42) }
pub extension BoxOps for Box {
    fn reveal(): Int { secret }
    fn choose(value: Int): String { "extension" }
    fn display(): String { "extension display" }
}
pub extension ListOps<T> for List<T> {
    fn transform<R>(body: (T) -> R): List<R> {
        val result: List<R> = []
        for item in this { result.add(body(item)) }
        result
    }
}
pub extension ExtraListOps<T> for List<T> {
    fn transform<R>(body: (T) -> R): List<R> { [] }
}
''')
        (self.lib / 'src/lib.ly').write_text('pub import self.net.{ListOps as ChainOps}\n')
        imports = '''import cfg.{ChainOps as Chain}
import cfg.net.{BoxOps}
import cfg.net.{box}
'''
        self.source.write_text(imports + '''fn main(): Unit {
    print(box().reveal())
    print(box().choose(1))
    print(box().display())
    val mapped = [1, 2].transform { it.toString() }.transform { it + "!" }
    print(mapped[1])
}
''')
        for options in ((), ('--release',)):
            self.assertEqual(self.command('run', self.app, *options).stdout, '42\nmember\nextension display\n2!\n')
        cases = (
            ('fn main(): Unit { [7].transform { it }; }', 'LYN0001', 'transform'),
            (imports + 'import cfg.net.{ExtraListOps}\nfn main(): Unit { [7].transform { it }; }', 'LYT0019', 'ExtraListOps'),
            ('import cfg.net.{Box}\nextension Bypass for Box { fn read(): Int { this.secret } }\nfn main(): Unit {}', 'LYN0003', 'secret'),
            ('extension Unbound<T> for Int { fn read(): Int { this } }\nfn main(): Unit {}', 'LYG0002', 'T'),
        )
        for body, code, detail in cases:
            self.source.write_text(body)
            error, = json.loads(self.command('check', self.app, '--diagnostics=json', ok=False).stderr)
            self.assertEqual(error['code'], code)
            self.assertIn(detail, error['message'])

    def test_public_signature_cannot_export_a_private_type_or_constraint(self):
        lib = self.lib / 'src/net/types.ly'
        self.source.write_text('import cfg.{make as aliased}\nfn main(): Unit {}')
        (self.lib / 'src/lib.ly').write_text('pub import self.net.{build as make}\n')
        declarations = '''struct Hidden(val value: Int)
trait Internal { fn read(): Int }
pub struct Box<Item>(pub val item: Item)
'''
        for leak, detail in (
            ('pub fn build<T>(value: T): List<(Hidden) -> Box<T>> { [] }', 'Hidden'),
            ('pub fn build(value: Box<Hidden>?): Unit {}', 'Hidden'),
            ('pub fn build<T: Internal>(value: T): Unit {}', 'Internal'),
            ('pub enum Leak<Item> { Value(Result<Item,Hidden>) }', 'Hidden'),
            ('pub class Leak<Item>(pub val field: Box<Hidden>)', 'Hidden'),
            ('pub trait Leak { fn hidden(): Hidden }', 'Hidden'),
            ('pub extension Leak for Hidden { fn read(): Int { value } }', 'Hidden'),
        ):
            with self.subTest(leak=leak):
                entry = '' if leak.startswith('pub fn build') else 'pub fn build(): Unit {}\n'
                lib.write_text(declarations + entry + leak)
                error, = json.loads(self.command('check', self.app, '--diagnostics=json', ok=False).stderr)
                self.assertEqual(error['code'], 'LYN0003')
                self.assertIn(detail, error['message'])
                self.assertIn('types.ly', error['file'])
        lib.write_text(declarations + '''pub class Counter(val state: Hidden) { pub fn read(): Int { state.value } }
pub fn build(): Counter { Counter(Hidden(42)) }
''')
        self.source.write_text('import cfg.{make as aliased}\nfn main(): Unit { print(aliased().read()) }')
        self.assertEqual(self.command('run', self.app).stdout, '42\n')

    def test_any_preserves_canonical_types_and_gc_roots(self):
        (self.lib / 'src/net/box.ly').write_text('''
pub class Box<Item>(pub var value: Item)
pub fn erase<T>(value: Box<T>): Any { value }
''')
        (self.lib / 'src/lib.ly').write_text('pub import self.net.{Box as Exported}\n')
        self.source.write_text('''
import cfg.{Exported as Original}
import cfg.net.{erase}
class Box<Item>(val value: Item)
async fn main(): Unit {
    val original = Original("same")
    val boxed = erase(original)
    val retained = erase(Original("only via Any"))
    await blocking { gcCollect() }
    print(boxed is Original<String>)
    print(boxed is Original<Int>)
    print(boxed is Box<String>)
    match boxed as? Original<String> {
        None => print("lost"),
        Some(value) => { value.value = "changed"; print(value == original) },
    }
    print(original.value)
    print((retained as? Original<String>)?.value ?? "lost")
}
''')
        for options in ((), ('--release',)):
            self.assertEqual(self.command('run', self.app, *options).stdout, 'true\nfalse\nfalse\ntrue\nchanged\nonly via Any\n')

    def test_dyn_dispatches_heterogeneous_methods_including_async(self):
        (self.lib / 'src/net/render.ly').write_text('''
pub trait Renderer { fn render(prefix: String): String; async fn later(): String }
pub trait Label { fn render(prefix: String): String }
pub struct Badge(pub val name: String) {
    pub fn render(prefix: Int): String { "unexpected overload" }
    pub fn render(prefix: String): String { prefix + name }
    pub async fn later(): String { print("activated"); name }
}
pub class Lamp<T>(pub var name: T) {
    pub fn render(prefix: String): String { prefix + "lamp" }
    pub async fn later(): String { "dynamic async" }
}
pub class Hidden() { fn render(prefix: String): String { prefix } }
pub fn pack<T: Renderer>(value: T): dyn Renderer { value as dyn Renderer }
''')
        (self.lib / 'src/lib.ly').write_text('pub import self.net.{Renderer as View}\n')
        imports = '''import cfg.{View}
import cfg.net.{Label}
import cfg.net.{Badge}
import cfg.net.{Lamp}
import cfg.net.{Hidden}
import cfg.net.{pack}
'''
        self.source.write_text(imports + '''
async fn main(): Unit {
    val all: List<dyn View> = [Badge("badge") as dyn View, pack(Lamp(42))]
    await blocking { gcCollect() }
    for item in all { print(item.render(prefix: "view ")) }
    val task = all[0].later()
    print("cold"); print(await task); print(await all[1].later())
    val simple = all[1] as dyn Label
    print(simple.render("simple "))
    val boxed: Any = all[0]
    match boxed as? dyn View {
        Some(view) => print(view.render("boxed ")),
        None => print("lost"),
    }
}
''')
        for options in ((), ('--release',)):
            self.assertEqual(self.command('run', self.app, *options).stdout,
                             'view badge\nview lamp\ncold\nactivated\nbadge\ndynamic async\nsimple lamp\nboxed badge\n')
        for body, code, detail in (
            ('val item: dyn View = Badge("badge")', 'LYT0001', 'dyn Renderer'),
            ('val items = [Badge("badge")]; val refs: List<dyn View> = items', 'LYT0001', 'List<dyn Renderer>'),
            ('val hidden = Hidden() as dyn Label', 'LYG0005', 'render'),
        ):
            self.source.write_text(imports + 'fn main(): Unit { ' + body + ' }')
            error, = json.loads(self.command('check', self.app, '--diagnostics=json', ok=False).stderr)
            self.assertEqual(error['code'], code)
            self.assertIn(detail, error['message'])


if __name__ == '__main__':
    unittest.main()
