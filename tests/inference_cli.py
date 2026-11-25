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
        (self.root / 'src/main.ly').write_text(source, encoding='utf-8', newline='\n')
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

    def test_inferred_join_is_independent_of_branch_order(self):
        self.runBoth('''fn either(flag: Bool, narrow: Int8, wide: Int): Int {
    val first = if flag { narrow } else { wide }
    val second = if flag { wide } else { narrow }
    val list = [narrow, wide]
    val reverse = [wide, narrow]
    val recovered = try { wide } catch { narrow }
    val patterned = match flag { false => narrow, true => wide }
    first + second + list[0] + reverse[0] + recovered + patterned
}
fn returned(flag: Bool, narrow: Int8, wide: Int) { if flag { return narrow }; wide }
fn optional(flag: Bool) { if flag { return None }; "text" }
async fn main(): Unit {
    val narrow: Int8 = 2
    val wide: Int = 300
    print(either(false, narrow, wide))
    print(returned(false, narrow, wide))
    print(optional(true) == None)
    print(optional(false) ?? "missing")
    val items = if false { [] } else { [narrow] }
    print(items[0])
    val action = if false { (value) => { value } } else { (value: Int) => { value + 1 } }
    print(action(6))
    val task = async { if true { return narrow }; wide }
    val value: Int = await task
    print(value)
    val selected = await select { sleepMs(0) => narrow, sleepMs(0) => wide }
    val widened: Int = selected + wide
    print(widened)
}''', '906', '300', 'true', 'text', '2', '7', '2', '302')

    def test_inferred_join_does_not_box_or_search_user_conversions(self):
        self.reject('''struct Label(val text: String)
implicit (text: String): Label { Label(text) }
fn main(): Unit { val answer = if false { "text" } else { Label("other") } }
''', 'LYT0001', 'Label')
        self.reject('''fn main(): Unit {
    val raw: Any = "text"
    val values = [raw, "other"]
}
''', 'LYT0001', 'Any')

    def test_pattern_and_any_facts_follow_live_paths(self):
        self.runBoth('''async fn length(name: String?, value: Any): Int {
    if let None = name { return 0 }
    if !(value is String) { return 1 }
    await blocking { gcCollect() }
    if value is String && value.size > 0 { return name.size + value.size }
    name.size
}
fn matchLength(name: String?): Int {
    match name { None => { return 0 }, Some(_) => { print(name.size); } }
    name.size
}
fn invoke(value: Any): Int {
    if value is (() -> Int) { return value() }
    0
}
async fn main(): Unit {
    print(await length(None, "abc"))
    print(await length("ok", 0))
    print(await length("ok", "abc"))
    print(matchLength("yes"))
    print(matchLength(None))
    print(invoke(() => { 42 }))
}''', '0', '1', '5', '3', '3', '0', '42')
        self.reject('''fn main(): Unit {
    var value: Any = "text"
    if value is String { print(value.size) }
}''', 'LYN0001', 'size')

    def test_warnings_use_one_json_response_and_point_at_each_source_expression(self):
        source = '''const First: Int = 1
async fn work(): Unit {}
fn parse(): Result<Int,String> { Ok(1) }
async fn main(): Unit {
    work(); parse(); val _ = work(); val _ = parse()
    spawn work(); parallel {}
    val value = match 2 { First | 1 | 2 => 0, 1..2 => 1, _ => 2 }
    val guarded = match true { _ if false => 0, true => 1, false => 2 }
}
'''
        result = self.cli('check', source, '--diagnostics=json')
        self.assertEqual(result.returncode, 0, result.stderr)
        diagnostics = json.loads(result.stderr)
        self.assertEqual([item['code'] for item in diagnostics], ['LYW0001', 'LYW0001', 'LYW0002', 'LYW0002'])
        self.assertTrue(all(item['severity'] == 'warning' for item in diagnostics))
        self.assertEqual([source[item['range']['start']['offset']:item['range']['end']['offset']] for item in diagnostics],
                         ['work()', 'parse()', '1', '1..2'])


if __name__ == '__main__':
    unittest.main()
