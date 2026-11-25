import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from platform_cli import executable


COMPILER = Path(sys.argv.pop(1)).resolve()


class SyntaxCases(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.package = Path(directory.name)
        (self.package / 'src').mkdir()
        self.main = self.package / 'src/main.ly'

    def cli(self, command, *options):
        return subprocess.run([COMPILER, command, self.package, *options], capture_output=True, timeout=30)

    def native(self, source, expected):
        self.main.write_bytes(source.encode())
        for flags in ([], ['--release']):
            with self.subTest(flags=flags):
                app = executable(self.package / 'app')
                built = self.cli('build', *flags, '-o', app)
                self.assertEqual(built.returncode, 0, built.stderr.decode())
                ran = subprocess.run([app], capture_output=True, text=True, encoding='utf-8', timeout=10)
                self.assertEqual((ran.returncode, ran.stdout, ran.stderr), (0, expected, ''))

    def reject(self, source, code, span):
        data, marker = source.encode(), span.encode()
        self.main.write_bytes(data)
        checked = self.cli('check', '--diagnostics=json')
        self.assertNotEqual(checked.returncode, 0, source)
        issue, = json.loads(checked.stderr)
        self.assertEqual(issue['code'], code, issue)
        self.assertEqual(Path(issue['file']).name, self.main.name)
        location = issue['range']
        start = data.rindex(marker)
        self.assertEqual((location['start']['offset'], location['end']['offset']),
                         (start, start + len(marker)), issue)

    def test_pattern_loops_preserve_fresh_bindings_and_cleanup(self):
        self.native('''class Cursor(var count: Int) {
    fn next(): Int? { count += 1; if count < 3 { count } else { None } }
}
fn main(): Unit {
    val cursor = Cursor(0)
    val saved: List<() -> Int> = []
    while let Some(value) = cursor.next() {
        defer { print(value) }
        saved.add { value }
        continue
    }
    gcCollect()
    print(cursor.count); print(saved[0]()); print(saved[1]())
}''', '1\n2\n3\n1\n2\n')
        self.reject('fn main(): Unit { val items: List<Int?> = [1]; for Some(value) in items {} }', 'LYT0012', 'Some(value)')
        self.reject('fn main(): Unit { val value: Int? = 1; while let Some(item) = value { break }; print(item) }', 'LYN0001', 'item')

    def test_bitwise_precedence_width_and_generics(self):
        self.native('''fn identity<T>(value: T): T { value }
fn main(): Unit {
    val values: List<List<Int>>=[[2]]
    print(identity<List<List<Int>>>(values)[0][0] << 1 + 2)
    var mask: UInt8 = 254
    mask <<= 1; mask ^= 3; mask &= 15; mask |= 16; mask >>= 1
    print(mask)
    print(~mask)
    val signed: Int8 = -16
    print(signed >> 2)
    print(3 & 1 == 1)
    print(match 1 { 0 | 1 => 42, _ => 0 })
}''', '16\n15\n240\n-4\ntrue\n42\n')
        self.reject('fn main(): Unit { print(true & false) }', 'LYT0006', '&')
        self.main.write_text('fn main(): Unit { val narrow: Int8 = 1; print(narrow << 8) }')
        bad = self.cli('run')
        self.assertNotEqual(bad.returncode, 0, bad.stdout)
        self.assertIn(b'shift count out of range', bad.stderr)

    def test_escaped_keywords_and_contextual_names(self):
        self.native('''
fn `if`(value: Int): Int { value + 1 }
fn init(value: Int): Int { value }
fn blocking(value: Int): Int { value + 2 }
fn apply(action: (Int) -> Int): Int { action(3) }
class Reader(val `this`: Int) { fn read(): Int { this.`this` } }
enum State { Idle, Ready }
fn main(): Unit {
    val `None` = 2
    val it = 7
    val _slot2 = 1_000
    val slot2 = 3
    val Slot2 = 4
    print(`if`(init(`None`)))
    print(blocking(it))
    print(apply { it * 2 })
    print(Reader(_slot2 + slot2 * Slot2).read())
    print(match State.Idle { `Idle` => match `Idle` { Idle => 8, Ready => 0 } })
    print(match 5 { `_` => `_` })
}
''', '3\n9\n6\n1012\n8\n5\n')

    def test_lexical_rejections_have_original_utf8_spans(self):
        for body, code, span in (
            ('val if = 1', 'LYP0001', 'if'),
            ('val this = 1', 'LYP0001', 'this'),
            ('val `9 = 1', 'LYL0001', '`'),
            ('val 名 = 1', 'LYL0001', '名'),
            ('val i/* no joining */f = 1', 'LYP0001', 'f'),
            ('val count = 2 /* unterminated', 'LYL0001', '/*'),
        ):
            with self.subTest(body=body):
                self.reject('// 中文🙂\r\nfn main(): Unit { ' + body + ' }', code, span)

    def test_lf_crlf_comments_and_block_tails(self):
        source = '''
fn note(value: Int): Int { print(value); value }
fn discard(): Unit { note(3); }
fn keep(): Int { return note(4); }
fn early(): Unit { return
    print(99)
}
fn run(action: () -> Int = () => { 5 }): Int { action() }
fn main(): Unit {
    print(1 + /* unfinished expression
        */ 2)
    print((4
        + 5))
    print(if false { 0 }
        else { 6
        })
    discard()
    print(keep())
    early()
    print(run { 7 })
    run() /* next block is not a lambda
        */ { print(8) }
    print(run(() => { note(10)
        11 }))
    print([1,
        2]
        .size)
}
'''
        for newline in ('\n', '\r\n'):
            with self.subTest(newline=newline):
                self.native(source.replace('\n', newline), '3\n9\n6\n3\n4\n4\n7\n8\n10\n11\n2\n')

    def test_newline_and_semicolon_rejections(self):
        for source, code, span in (
            ('fn main(): Unit { val sum = 1 /* newline\n*/ + 2 }', 'LYP0001', '+'),
            ('fn main(): Unit { val items = [1\n2] }', 'LYP0001', '2'),
            ('fn main(): Unit { print(1\n2) }', 'LYP0001', '2'),
            ('fn main(): Unit { val count: Int = if true { 1 } }', 'LYT0005', 'if true { 1 }'),
            ('fn total(): Int { 1 + 2; } fn main(): Unit {}', 'LYT0001', ';'),
        ):
            with self.subTest(source=source):
                self.reject(source, code, span)

    def test_control_flow_blocks_do_not_bind_as_trailing_lambdas(self):
        self.native('''
fn ready(): Bool { true }
fn stillRunning(index: Int): Bool { index < 1 }
fn items(): List<Int> { [1, 2] }
fn test<Item>(item: Item, predicate: (Item) -> Bool): Bool { predicate(item) }
fn main(): Unit {
    if ready() { print(1) }
    if (test<List<Int>>(items()) { it.size == 2 }) { print(2) }
    var index = 0
    while stillRunning(index) { index += 1 }
    print(index)
    for item in items() { print(item) }
    print(match ready() { true => 3, false => 0 })
}
''', '1\n2\n1\n1\n2\n3\n')

    def test_generic_call_requires_attached_angle_brackets(self):
        for expression, code, span in (
            ('identity<right>(7)', 'LYT0003', 'right'),
            ('identity <Int>(7)', 'LYP0001', '>'),
            ('identity<Int> (7)', 'LYP0001', '>'),
        ):
            with self.subTest(expression=expression):
                self.reject('fn identity<T>(value: T): T { value }\n'
                            f'fn main(): Unit {{ val right = 2; print({expression}) }}', code, span)

    def test_spread_fills_only_variadic_slots(self):
        self.native('''
fn gather(head: Int = 7, rest: Int...): Int { head * 10 + rest.size }
fn main(): Unit {
    print(gather(...[1, 2]))
    print(gather(3, ...[1, 2], 4))
}
''', '72\n33\n')
        self.reject('fn gather(head: Int, rest: Int...): Unit {}\n'
                    'fn main(): Unit { gather(...[1, 2]) }', 'LYT0002', 'gather')
        self.reject('fn takeMany(...values: Int): Unit {} fn main(): Unit {}', 'LYP0001', '...')

    def test_postfix_binds_before_negation_without_losing_minimum_literals(self):
        self.native('''
fn minByte(): Int8 { -128 }
fn minInt(): Int { -9223372036854775808 }
fn main(): Unit {
    print((-1).toInt8Checked() ?? 0)
    print(minByte())
    print(minInt())
}
''', '-1\n-128\n-9223372036854775808\n')
        self.reject('fn main(): Unit { val number = -1.toInt8Checked() }', 'LYT0001', '1.toInt8Checked()')

    def test_boxed_variadic_items_and_spread(self):
        self.native('''
fn countArgs(values: Any...): Int { values.size }
fn unbox<T>(value: Any): T? { value as? T }
fn genericCount<T>(values: T...): Int { values.size }
fn first(values: Any...): Int {
    val value = unbox<Int>(values[0]) ?? -1
    values[0] = "local"
    value
}
class Cursor()
fn main(): Unit {
    val ids = [2, 5]
    val empty: List<Int> = []
    print(countArgs(ids)); print(countArgs(...ids))
    print(countArgs(empty)); print(countArgs(...empty))
    print(countArgs(Cursor()))
    print(first(...ids)); print(ids[0]); print(genericCount<Any>(...ids))
    val data: Any = 42
    print(data is Int64 && !(data is UInt64))
    print(data as? Bool ?? false)
}
''', '1\n2\n1\n0\n1\n2\n2\n2\ntrue\nfalse\n')
        for program, code, span in (
            ('fn countArgs(values: Any...): Int { values.size }\nclass Cursor()\nfn main(): Unit { val cursor = Cursor(); countArgs(...cursor); }', 'LYT0001', '...cursor'),
            ('fn main(): Unit { val boxed: Any = 1; boxed == boxed; }', 'LYT0006', '=='),
            ('fn main(): Unit { val int = 1; int is Bool; }', 'LYT0021', 'is'),
            ('fn main(): Unit { val ints = [1, 2]; val all: List<Any> = ints }', 'LYT0001', 'ints'),
        ):
            with self.subTest(program=program):
                self.reject(program, code, span)

    def test_await_precedes_propagation_without_flattening_option(self):
        self.native('''
async fn find(fails: Bool): Result<Int?,String> { if fails { Err("lost") } else { Ok(None) } }
async fn probe(fails: Bool): Result<Int,String> {
    val first = await find(fails)?
    val second = (await find(false))?
    print("continued")
    Ok((first ?? 2) + (second ?? 3))
}
async fn main(): Unit {
    print(match await probe(false) { Ok(value) => value, Err(_) => 0 })
    print(match await probe(true) { Ok(_) => "unexpected", Err(error) => error })
}
''', 'continued\n5\nlost\n')
        declaration = 'async fn find(): Result<Int?,String> { Ok(None) }\n'
        self.reject(declaration + 'async fn bad(): Result<Int?,String> { Ok(await (find()?)) }\n'
                    'fn main(): Unit {}', 'LYT0013', '?')
        self.reject(declaration + 'async fn main(): Unit { defer await find() }', 'LYT0008', 'await')

    def test_member_of_propagated_value_needs_parentheses(self):
        self.native('''
fn length(value: String?): Int? { (value?).size }
fn main(): Unit { print(length(Some("lyra")) ?? 0); print(length(None) ?? 0) }
''', '4\n0\n')
        self.reject('fn length(value: String?): Int? { value? .size } fn main(): Unit {}', 'LYP0001', '.')


if __name__ == '__main__':
    unittest.main()
