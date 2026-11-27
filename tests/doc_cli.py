import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class Reference(unittest.TestCase):
    def setUp(self):
        workspace = tempfile.TemporaryDirectory()
        self.addCleanup(workspace.cleanup)
        self.root = Path(workspace.name) / '中文 项目'
        self.root.mkdir()

    def write(self, path, source, eol='\n'):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.replace('\n', eol).encode('utf-8'))
        return target

    def command(self, *args, ok=True):
        result = subprocess.run([COMPILER, 'doc', *args], cwd=self.root, capture_output=True, timeout=75,
                                env={**os.environ, 'LYRA_CACHE': str(self.root / 'cache')})
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr.decode('utf-8', errors='replace'))
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def test_package_exports_reexports_members_and_generated_api(self):
        self.write('lyra.json5', '{workspace: {members: ["data", "facade"]}}')
        self.write('data/lyra.json5', '{name:"data",version:"0.1.0",kind:"lib"}')
        self.write('facade/lyra.json5', '{name:"facade",version:"0.1.0",kind:"lib",'
                   'dependencies:{data:{name:"data",version:"0.1.0",path:"../data"}}}')
        self.write('data/src/nested/api.ly', '''/// 包装值。
pub struct Box<T: Eq>(
    /// 保存的值。
    pub val value: T
) {
    pub fn read(): T { value }
    fn hidden(): T { value }
}
/// Reads the value.
///
/// ```lyra
///   read(42)
/// ```
pub fn read(value: Int = 42): Int { value }
pub fn read(value: String): String { value }
fn read(value: Bool): Bool { value }
pub enum Event {
    /// Carries a code.
    Ready(code: Int),
    Waiting
}
pub extension Label for String {
    /// Returns the label length.
    val length: Int get() { this.size }
}
/// Blocked by the ordinary comment.
// Not documentation.
pub fn noDocs(): Int { 0 }
''', '\r\n')
        self.write('data/src/nested/.macros/emit.ly', '''/// Emits a public helper.
pub fn emit(target: String, argument: String): Syntax {
    syntaxCode("/// 文档来自宏生成源码。\\npub fn generated(): Int { 7 }")
}
''')
        facade = self.root / 'facade'
        self.write('facade/src/lib.ly', '''pub import data.nested.{read as fetch, Box as Container, Label}
pub import macro data.nested.emit as make
/// Target documentation crosses a multiline macro prefix.
#make(
    export generated
)
pub fn target(): Unit {}
''')
        output = self.command(facade, '--offline').stdout
        data = json.loads(output)
        self.assertEqual(data['schemaVersion'], 1)
        self.assertEqual(self.command(facade, '--offline', '--locked').stdout, output)
        self.assertEqual([module['name'] for module in data['modules']], ['facade'])
        entries = data['entries']
        ids = [entry['id'] for entry in entries]
        self.assertEqual(ids, sorted(set(ids)))
        overloads = [entry for entry in entries if entry['name'] == 'fetch']
        self.assertEqual({entry['signature'] for entry in overloads}, {'pub fn fetch(value: Int = 42): Int', 'pub fn fetch(value: String): String'})
        documented, = [entry for entry in overloads if entry['documentation']]
        self.assertIn('```lyra\n  read(42)\n```', documented['documentation'])
        self.assertEqual(documented['parameters'][0]['default'], '42')
        self.assertEqual(documented['source']['file'], 'src/lib.ly')
        self.assertTrue(documented['originId'].startswith('data.nested::read:'))
        container, = [entry for entry in entries if entry['kind'] == 'struct']
        self.assertEqual(container['signature'], 'pub struct Container<T: Eq>')
        members = {entry['name']: entry for entry in entries if entry.get('ownerId') == container['id']}
        self.assertEqual(set(members), {'value', 'read', 'Container'})
        self.assertEqual(members['value']['documentation'], '保存的值。')
        self.assertEqual(members['value']['source']['file'], 'src/nested/api.ly')
        length, = [entry for entry in entries if entry['kind'] == 'property']
        self.assertEqual(length['receiver'], 'String')
        self.assertEqual((length['signature'], length['visibility']), ('val length: Int', 'public'))
        generated, = [entry for entry in entries if entry['generated']]
        self.assertEqual((generated['name'], generated['documentation']), ('generated', '文档来自宏生成源码。'))
        self.assertEqual(generated['source']['line'], 4)
        target, = [entry for entry in entries if entry['name'] == 'target']
        self.assertIn('multiline macro prefix', target['documentation'])
        macro, = [entry for entry in entries if entry['kind'] == 'macro']
        self.assertEqual(macro['signature'], '#make')
        self.assertIn('pub fn emit(', macro['implementationSignature'])
        self.assertEqual(macro['documentation'], 'Emits a public helper.')
        workspace = json.loads(self.command(self.root).stdout)
        self.assertIn('data.nested', {module['name'] for module in workspace['modules']})
        by_id = {entry['id']: entry for entry in workspace['entries']}
        self.assertEqual(by_id[documented['originId']]['name'], 'read')
        ready, = [entry for entry in workspace['entries'] if entry['name'] == 'Ready']
        self.assertEqual(ready['parameters'][0]['name'], 'code')
        self.assertEqual(ready['documentation'], 'Carries a code.')
        undocumented, = [entry for entry in workspace['entries'] if entry['name'] == 'noDocs']
        self.assertIn(undocumented['id'], workspace['missingDocumentation'])

    def test_sdk_contains_unimported_packages_builtins_and_grammar(self):
        target = self.root / '参考' / 'sdk.json'
        stdout = self.command('--stdlib').stdout
        self.assertEqual(self.command('--stdlib', '-o', target).stdout, b'')
        self.assertEqual(target.read_bytes(), stdout)
        data = json.loads(stdout)
        entries = {item['qualifiedName']: item for item in data['entries']}
        module_names = {module['name'] for module in data['modules']}
        standard = COMPILER.parent.parent / 'share/lyra/std'
        expected = {'std.' + folder.name for folder in standard.iterdir() if (folder / 'src').is_dir()}
        self.assertEqual(module_names, expected | {'std.meta', 'builtin'})
        self.assertFalse(data['missingDocumentation'], '\n'.join(data['missingDocumentation']))
        self.assertTrue(entries['builtin::Option.unwrap']['documentation'])
        self.assertEqual(entries['builtin::Bytes.fromHex']['parameters'][0]['type'], 'String')
        self.assertEqual(entries['builtin::Error.cause']['kind'], 'property')
        self.assertEqual(entries['builtin::syntaxDoc']['signature'], 'fn syntaxDoc(node: Syntax): String')
        self.assertEqual(entries['builtin::syntaxComment']['context'], 'macro')
        self.assertEqual(entries['builtin::ChannelReceiveResult.Received']['parameters'][0]['type'], 'T')
        self.assertTrue(entries['std.core::ResultOps.orElse']['documentation'])
        self.assertIn('```lyra', entries['std.core::OptionOps.toResult']['documentation'])
        self.assertEqual(entries['std.meta::assert']['kind'], 'expressionMacro')
        self.assertIn('pub', data['syntax']['keywords'])
        self.assertIn('extension', data['syntax']['contextualKeywords'])
        operators = {(operator['spelling'], operator['fixity']): operator for operator in data['syntax']['operators']}
        self.assertEqual(operators['??', 'infix']['associativity'], 'right')
        self.assertEqual(operators['..<', 'infix']['associativity'], 'none')
        self.assertGreater(operators['*', 'infix']['precedence'], operators['+', 'infix']['precedence'])

    def test_overload_ids_follow_types_not_positions_or_parameter_spelling(self):
        self.write('lyra.json5', '{name:"sample",version:"0.1.0",kind:"lib"}')
        original = '''pub struct Key(pub val value: Int)
type Alias = Key
pub fn lookup<T>(input: Alias, extra: T): Int { input.value }
pub fn lookup(input: String): Int { input.size }
'''
        self.write('src/lib.ly', original)

        def overloads():
            entries = json.loads(self.command(self.root).stdout)['entries']
            return {item['id']: item['signature'] for item in entries if item['name'] == 'lookup'}

        first = overloads()
        self.write('src/lib.ly', '\n/// More lines.\n' + original.replace('<T>', '<U>').replace(': T)', ': U)')
                   .replace('input: Alias', 'key: Key').replace('input.value', 'key.value'), '\r\n')
        second = overloads()
        self.assertEqual(len(first), 2)
        self.assertEqual(first.keys(), second.keys())
        self.assertNotEqual(first, second)

    def test_failed_check_keeps_previous_output_and_emits_diagnostics(self):
        self.write('lyra.json5', '{name:"broken",version:"0.1.0",kind:"lib"}')
        source = self.write('src/lib.ly', 'pub fn bad(): Int { "str" }\n')
        target = self.write('existing.json', '{"previous":true}\n')
        result = self.command(self.root, '-o', target, '--diagnostics=json', ok=False)
        self.assertEqual(result.stdout, b'')
        self.assertEqual(json.loads(result.stderr)[0]['code'], 'LYT0001')
        self.assertEqual(target.read_bytes(), b'{"previous":true}\n')
        self.write('src/lib.ly', 'pub fn good(): Int { 42 }\n')
        result = self.command(self.root, '-o', source, '--diagnostics=json', ok=False)
        self.assertEqual(json.loads(result.stderr)[0]['code'], 'LYI0001')
        self.assertIn(b'pub fn good', source.read_bytes())


if __name__ == '__main__':
    unittest.main()
