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
        (self.lib / 'src/lib.ly').write_text('pub import self.net.port as defaultPort\n')
        self.source = self.app / 'src/main.ly'
        self.source.write_text('import cfg.defaultPort as port\nfn hidden(): Int { 1 }\nfn main(): Unit { print(port() * 2) }\n')

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
        (self.lib / 'tests/port.ly').write_text('import self.defaultPort\nfn main(): Unit { print(defaultPort()) }')
        self.assertIn('21\nok lib/port.ly', self.command('test', self.lib).stdout)

    def test_private_import_alias_and_cycles(self):
        cases = [('import cfg.net.hidden', 'LYN0003'), ('import cfg.net.missing', 'LYN0003'),
                 ('import other.net.port', 'LYN0003'), ('import cfg.net.port as main', 'LYN0002')]
        for declaration, code in cases:
            self.source.write_text(declaration + '\nfn main(): Unit {}')
            diagnostic, = json.loads(self.command('check', self.app, '--diagnostics=json', ok=False).stderr)
            self.assertEqual(diagnostic['code'], code)
            self.assertEqual(diagnostic['range']['start']['line'], 1)
        (self.lib / 'src/lib.ly').write_text('pub import self.net.loop as defaultPort')
        (self.lib / 'src/net/alias.ly').write_text('pub import self.defaultPort as loop')
        self.source.write_text('import cfg.defaultPort\nfn main(): Unit {}')
        diagnostic, = json.loads(self.command('check', self.app, '--diagnostics=json', ok=False).stderr)
        self.assertEqual(diagnostic['code'], 'LYN0004')

    def test_workspace_tests_use_member_not_sibling_entry(self):
        (self.root / 'lyra.json5').write_text('{workspace: {members: ["app", "lib"]}}')
        self.command('check', self.root)
        self.assertEqual(self.command('run', self.app, '--locked').stdout, '42\n')
        self.command('run', self.root, ok=False)

    def test_struct_patterns_resolve_import_alias_and_check_field_visibility(self):
        (self.lib / 'src/net/box.ly').write_text('pub struct Box(pub val number: Int, val secret: Int)\n'
                                                 'pub fn makeBox(): Box { Box(42, 7) }\n')
        imports = 'import cfg.net.Box as Item\nimport cfg.net.makeBox\n'
        self.source.write_text(imports + 'fn main(): Unit { print(match makeBox() { Item(number: number) => number }) }\n')
        self.assertEqual(self.command('run', self.app).stdout, '42\n')
        self.source.write_text(imports + 'fn main(): Unit { match makeBox() { Item(secret: _) => {} } }\n')
        error, = json.loads(self.command('check', self.app, '--diagnostics=json', ok=False).stderr)
        self.assertEqual(error['code'], 'LYN0003')
        self.assertIn('secret', error['message'])

    def test_enum_variants_and_implicit_conversion_across_modules(self):
        (self.lib / 'src/net/status.ly').write_text('''
enum Internal { Wait }
pub enum Status { Wait, Ready(Int) }
pub implicit (value: Status): Int { match value { Wait => 0, Ready(port) => port } }
''')
        self.source.write_text('''
import cfg.net.Status as Endpoint
fn identity<T>(value: T): T { value }
fn show(value: Int): Unit { print(value) }
fn main(): Unit {
    show(identity(Endpoint.Ready(42)))
    print(match identity(Endpoint.Wait) { Endpoint.Wait => 1, Ready(_) => 0 })
}
''')
        self.assertEqual(self.command('run', self.app, '--release').stdout, '42\n1\n')
        self.source.write_text('import cfg.net.Internal\nfn main(): Unit {}')
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
import cfg.net.Status as Endpoint
import cfg.net.wrap
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
        self.source.write_text('import cfg.net.Status as Endpoint\nfn main(): Unit { Endpoint<Int>.hidden(); }')
        diagnostic, = json.loads(self.command('check', self.app, '--diagnostics=json', ok=False).stderr)
        self.assertEqual(diagnostic['code'], 'LYN0003')

    def test_generic_enum_reexport_construct_and_match(self):
        (self.lib / 'src/net/load.ly').write_text('''
pub struct Entry<Item>(pub val value: Item)
pub enum Load<Item> { Pending, Ready(entry: Entry<Item>, next: Load<Item>), Failed(String) }
pub fn ready<Item>(item: Item): Load<Item> { Load.Ready(Entry(item), Load<Item>.Pending) }
''')
        (self.lib / 'src/lib.ly').write_text('pub import self.net.Load as LoadState\n')
        imports = '''import cfg.LoadState as State
import cfg.net.Entry
import cfg.net.ready
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


if __name__ == '__main__':
    unittest.main()
