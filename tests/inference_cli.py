import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class Inference(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        (self.root / 'src').mkdir()

    def cli(self, command, source, *options):
        (self.root / 'src/main.ly').write_text(source, encoding='utf-8')
        return subprocess.run([COMPILER, command, self.root, *options], capture_output=True, text=True, encoding='utf-8', timeout=30)

    def runBoth(self, source, *lines):
        for profile in ([], ['--release']):
            result = self.cli('run', source, *profile)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), list(lines))

    def reject(self, source, code, fragment):
        result = self.cli('check', source, '--diagnostics=json')
        self.assertNotEqual(result.returncode, 0, source)
        diagnostic, = json.loads(result.stderr)
        self.assertEqual(diagnostic['code'], code, diagnostic)
        self.assertIn(fragment, diagnostic['message'])

    def test_private_functions_and_methods_learn_their_results_from_bodies(self):
        self.runBoth('''fn calc(early: Bool) { if early { return later(7) }; later(6) + 1 }
fn later(value: Int) { value * 6 }
fn unwrap<Value>(value: Value) { value }
fn printDrop(value: Int) { print(value); }
fn choose(value: Int) { value + 1 }
fn choose(value: String) { value.size }
class Box<Value>(val value: Value) {
    fn get(early: Bool) { if early { return value }; value }
    static fn twice(value: Int) { later(value) * 2 }
}
async fn work() { await blocking { 55 } }
async fn main(): Unit {
    val action: (Bool) -> Int = calc
    print(action(true))
    print(calc(false))
    print(Box("text").get(false))
    print(Box<Int>.twice(7))
    printDrop(unwrap(18))
    print(choose("word"))
    print(choose(21))
    print(await work())
}''', '42', '37', 'text', '84', '18', '4', '22', '55')

    def test_annotations_required_at_exports_entry_and_recursive_cycles(self):
        for declaration in ('pub fn exported() { 2 }', 'class Box { pub fn read() { 2 } }'):
            self.reject(declaration + '\nfn main(): Unit {}', 'LYP0001', '标注返回类型')
        self.reject('fn main() {}', 'LYP0001', '标注返回类型')
        for declarations in (
            'fn loop(value: Int) { loop(value) }\nfn loop(value: String) { value }',
            'fn first() { second() }\nfn second() { first() }',
            'fn first() { second() }\nfn second(): Int { first() }',
            'class Box { fn read() { this.read() } }',
        ):
            self.reject(declarations + '\nfn main(): Unit {}', 'LYT0020', '递归')

    def test_body_inference_does_not_use_call_site_type(self):
        self.reject('fn number() { 7 }\nfn main(): Unit { val byte: Int8 = number() }', 'LYT0001', 'Int8')
        self.reject('fn mixed(flag: Bool) { if flag { return 4 }; "tail" }\nfn main(): Unit {}', 'LYT0001', 'String')
        self.reject('fn absent() { None }\nfn main(): Unit { val name: String? = absent() }', 'LYT0010', 'Option')

    def test_narrowing_after_early_exit_and_across_await_uses_the_snapshot(self):
        self.runBoth('''class User(var name: String?)
async fn length(optional: String?, user: User): Int {
    if optional == null { return 0 }
    val snapshot = user.name
    if !(snapshot != null) { return optional.size }
    user.name = null
    await blocking { gcCollect() }
    if null != optional && snapshot.size >= 2 { return optional.size + snapshot.size }
    99
}
fn either(text: String?): Int {
    if text == null || text.size == 0 { 0 } else { text.size }
}
fn call(action: ((Int) -> Int)?): Int {
    if action == null { return 0 }
    action(6)
}
fn existing(text: String?): Int {
    if text == null { return 0 }
    val optional: String? = text
    (text ?? "fallback").size + (optional?.size ?? 0)
}
async fn main(): Unit {
    print(await length(None, User("yes")))
    print(await length("ok", User(None)))
    print(await length("ok", User("yes")))
    print(either("abc"))
    print(either(None))
    print(call(Some((value) => { value * 7 })))
    print(existing("abc"))
}''', '0', '2', '5', '3', '0', '42', '6')

    def test_narrowing_does_not_escape_the_path_or_apply_to_mutable_sources(self):
        for body in (
            'var text = value; if text == null { return }; print(text.size)',
            'if user.name == null { return }; print(user.name.size)',
            'if gate { if value == null { return } }; print(value.size)',
            'while gate { if value == null { return }; break }; print(value.size)',
            'try { if value == null { throw Exception("missing") } } catch {}; print(value.size)',
            'val work = blocking { if value == null { return }; print(value.size) }; print(value.size)',
        ):
            self.reject('class User(var name: String?)\n'
                        'fn check(value: String?, user: User, gate: Bool): Unit { ' + body + ' }\n'
                        'fn main(): Unit {}', 'LYN0001', 'size')


if __name__ == '__main__':
    unittest.main()
