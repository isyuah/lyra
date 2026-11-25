import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class Constants(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.app = self.root / 'app'
        self.lib = self.root / 'cfg'
        (self.app / 'src').mkdir(parents=True)
        (self.lib / 'src/internal').mkdir(parents=True)
        self.env = {**os.environ, 'LYRA_CACHE': str(self.root / 'cache')}

    def write(self, relative, text):
        path = self.root / relative
        path.write_text(text, encoding='utf-8', newline='\n')
        return path

    def cli(self, *args):
        return subprocess.run([COMPILER, *args], capture_output=True, text=True, encoding='utf-8', timeout=30, env=self.env)

    def diagnose(self, package, code, fragment):
        result = self.cli('check', package, '--diagnostics=json')
        self.assertNotEqual(result.returncode, 0, result.stdout)
        diagnostic, = json.loads(result.stderr)
        self.assertEqual(diagnostic['code'], code, diagnostic)
        self.assertIn(fragment, diagnostic['message'])
        return diagnostic

    def test_block_constants_are_scoped_and_instantiated_with_generic_types(self):
        self.write('app/src/main.ly', '''const Base: Int = 7
fn empty<T>(): FrozenList<T> {
    const Empty: FrozenList<T> = FrozenList<T>([])
    Empty
}
fn delta() {
    const Local: Int = Base + 1
    val read = () => { Local }
    const Chosen: Bool = { val candidate = Local; candidate > 0 }
    match true { Chosen => read(), false => 0 }
}
const Count: Int = delta()
fn main(): Unit {
    print(Count)
    print(empty<Int>().size())
    print(empty<String>().size())
    const Base: Int = Count + 1
    if true { const Base: Int = Base + 1; print(Base) }
    print(Base)
}
''')
        result = self.cli('run', self.app)
        self.assertEqual((result.returncode, result.stdout.splitlines()), (0, ['8', '0', '0', '10', '9']), result.stderr)

    def test_frozen_list_is_a_shallow_snapshot_and_hashes_like_ctfe(self):
        self.write('app/src/main.ly', '''struct Item(val count: Int)
class Cell(var number: Int)
fn snapshot(): FrozenList<Item> {
    val input = [Item(3), Item(4)]
    val frozen = FrozenList(input)
    input[0] = Item(9)
    input.add(Item(10))
    frozen
}
const Items: FrozenList<Item> = snapshot()
const Digest: Int = Items.hashCode()
const Equal: Bool = Items == FrozenList([Item(3), Item(4)])
const Sum: Int = {
    var total = 0
    for Item(count: count) in Items { total += count }
    total
}
fn main(): Unit {
    print(Items == snapshot())
    print(Items != FrozenList([Item(4), Item(3)]))
    print(Items != FrozenList([Item(3)]))
    print(Digest == snapshot().hashCode())
    print(Equal)
    print(Sum)
    print(Items[0].count)
    val cell = Cell(1)
    val source = [cell]
    val view = FrozenList(source)
    cell.number = 2
    source[0] = Cell(3)
    print(view[0].number)
}
''')
        for mode in ([], ['--release']):
            result = self.cli('run', self.app, *mode)
            self.assertEqual((result.returncode, result.stdout.splitlines()), (0, ['true'] * 5 + ['7', '3', '2']), result.stderr)

    def test_block_constant_cannot_capture_or_escape_mutable_values(self):
        cases = [
            ('fn bad(value: Int): Unit { const Local: Int = value }', 'LYCT0001', '运行时绑定'),
            ('class Holder(val value: Int) { fn bad(): Unit { const Local: Int = value } }', 'LYCT0001', 'this'),
            ('fn bad(): Unit { const Local: Int = 1; Local = 2 }', 'LYT0004', '赋值'),
            ('fn bad(): Unit { const Local: Int = { print(1); 2 } }', 'LYCT0001', 'print'),
            ('fn bad(): Unit { const Local: Int = Local }', 'LYN0001', 'Local'),
            ('fn bad(): Unit { val frozen = FrozenList([1]); frozen[0] = 2 }', 'LYT0004', '赋值'),
            ('const Bad: FrozenList<List<Int>> = FrozenList([[1]])', 'LYCT0001', 'List'),
            ('class Item(val value: Int)\nconst Bad: FrozenList<Item?> = FrozenList([Some(Item(1))])', 'LYCT0001', 'Class'),
            ('const Bad: FrozenList<() -> Int> = FrozenList([() => { 1 }])', 'LYCT0001', '函数值'),
            ('class Item(val value: Int)\nfn bad(): Int { FrozenList([Item(1)]).hashCode() }', 'LYT0006', 'Hash'),
        ]
        for source, code, message in cases:
            with self.subTest(source=source):
                self.write('app/src/main.ly', source + '\nfn main(): Unit {}')
                self.diagnose(self.app, code, message)

    def test_structural_hash_charges_each_visited_element_to_the_budget(self):
        self.write('app/src/main.ly', '''enum Tree { Leaf, Node(FrozenList<Tree>) }
fn tree(): Tree {
    var node = Tree.Leaf
    for ignored in 0..<20 { node = Tree.Node(FrozenList([node, node])) }
    node
}
const Digest: Int = tree().hashCode()
fn main(): Unit {}
''')
        self.diagnose(self.app, 'LYCT0004', '步数')

    def test_bitwise_and_pattern_loops_have_native_ctfe_parity(self):
        self.write('app/src/main.ly', '''struct Pair(val value: Int8)
const fn bits(): Int8 {
    var count: Int8 = 0
    var next: Int8? = -2
    while let Some(value) = next { count ^= value; next = None }
    for Pair(value: input) in [Pair(7)] { count &= input }
    count <<= 2
    count >>= 1
    ~count
}
const calculated: Int8 = bits()
fn main(): Unit { print(calculated); print(bits()) }
''')
        result = self.cli('run', self.app, '--release')
        self.assertEqual((result.returncode, result.stdout), (0, '-13\n-13\n'), result.stderr)
        self.write('app/src/main.ly', 'const outside: Int = 1 >> -1\nfn main(): Unit {}')
        self.diagnose(self.app, 'LYCT0003', '位移量')

    def test_values_are_typed_and_inlined_after_cross_package_reexport(self):
        self.write('app/lyra.json5', '{name:"app",version:"0.1.0",dependencies:{cfg:{name:"cfg",version:"^0.1.0",path:"../cfg"}}}')
        self.write('cfg/lyra.json5', '{name:"cfg",version:"0.1.0",kind:"lib"}')
        self.write('cfg/src/internal/calc.ly', '''const base: Int8 = -2
pub const total: Int = score(from: base, through: 8)
pub const text: String = "值🙂=${total}"
pub const fn score(from: Int, through: Int): Int {
    if from > through { return 0 }
    var sum = 0
    for number in from..through { if number < 0 { continue }; sum += number }
    sum
}
''')
        self.write('cfg/src/api.ly', '''pub import self.internal.{total as entries}
pub import self.internal.{text}
pub import self.internal.{score}
''')
        self.write('app/src/main.ly', '''import cfg.{entries as count}
import cfg.{text}
import cfg.{score}
const maximum: UInt64 = 18446744073709551615
const rounded: Float32 = (16777216.0f32 + 1.0f32) - 16777216.0f32
const ok: Bool = true || (1 / 0 == 0)
const minimum: Int = -9223372036854775808
const widened: Float64 = maximum.toFloat64()
const sizeofText: Int = text.size
fn main(): Unit {
    print(count)
    print(text)
    print(score(1, 3))
    print(maximum)
    print(rounded)
    print(ok)
    print(minimum)
    print(widened > 0.0)
    print(sizeofText)
}
''')
        for mode in ([], ['--release']):
            with self.subTest(mode=mode):
                result = self.cli('run', self.app, *mode)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.splitlines(), ['36', '值🙂=36', '6', '18446744073709551615', '0', 'true', '-9223372036854775808', 'true', '5'])
        self.write('app/src/main.ly', 'import cfg.internal.{base}\nfn main(): Unit {}\n')
        self.diagnose(self.app, 'LYN0003', 'base')

    def test_constant_patterns_run_in_compile_time_functions(self):
        self.write('app/src/main.ly', '''
const Selected: Bool = true
const fn choose(value: Bool): Int {
    match value {
        Selected => 1,
        false => 0,
    }
}
const answer: Int = choose(true)
fn main(): Unit { print(answer) }
''')
        result = self.cli('run', self.app)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, '1\n')

    def test_invalid_constant_slice_reports_bounds(self):
        self.write('app/src/main.ly', 'const count: Int = [1, 2].slice(2..<1).size\nfn main(): Unit {}')
        diagnostic = self.diagnose(self.app, 'LYCT0003', '列表切片越界')
        self.assertEqual(diagnostic['range']['start']['column'], 27)

    def test_cross_module_cycle_reports_the_reference_that_closes_it(self):
        self.write('app/lyra.json5', '{name:"app",version:"0.1.0"}')
        (self.app / 'src/left').mkdir()
        (self.app / 'src/right').mkdir()
        self.write('app/src/left/first.ly', 'import self.right.{second}\npub const first: Int = second\n')
        self.write('app/src/right/second.ly', 'import self.left.{first}\npub const second: Int = first\n')
        self.write('app/src/main.ly', 'fn main(): Unit {}\n')
        diagnostic = self.diagnose(self.app, 'LYCT0002', '依赖环')
        self.assertEqual(Path(diagnostic['file']).name, 'second.ly')
        self.assertEqual(diagnostic['range']['start']['line'], 2)
        self.assertEqual(diagnostic['range']['start']['column'], 25)

    def test_purity_checks_unused_branches_and_constant_bindings_are_immutable(self):
        cases = [
            ('fn io(): Int { print(2); 2 }\nconst fn decide(): Int { if true { 1 } else { io() } }', 'LYCT0001', 'print'),
            ('const fn bad(): Unit { print(1) }', 'LYCT0001', 'print'),
            ('const bad: List<Int> = [1]', 'LYCT0001', 'List 只能'),
            ('struct Bag(val items: List<Int>)\nconst bad: Option<Bag> = Some(Bag([1]))', 'LYCT0001', 'List 只能'),
            ('class Item(val count: Int)\nconst bad: Item = Item(1)', 'LYCT0001', 'Class'),
            ('class Item(val count: Int)\nstruct Bag(val item: Item)\nconst bad: Option<Bag> = None', 'LYCT0001', 'Class'),
            ('class Item { init() { if false { print(1) } } }\nconst bad: Unit = { Item(); }', 'LYCT0001', 'print'),
            ('const bad: () -> Int = () => { 1 }', 'LYCT0001', '函数值'),
            ('struct Bag(val action: () -> Int)\nconst bad: Option<Bag> = None', 'LYCT0001', '函数值'),
            ('fn noisy(): Int { print(1); 1 }\nconst fn bad(): Int { val call = noisy; if false { call() } else { 2 } }', 'LYCT0001', 'print'),
            ('const fn bad(): Int { val call = () => { print(1); 1 }; 2 }', 'LYCT0001', 'print'),
            ('async fn pause(): Int { 1 }\nconst fn bad(): Int { val call = pause; 2 }', 'LYCT0001', '编译期'),
            ('extern fn foreign(value: Int): Int;\nconst fn bad(): Int { val call = foreign; 2 }', 'LYCT0001', '外部'),
            ('const value: Int = 1\nfn attempt(): Unit { value = 2 }', 'LYT0004', '赋值'),
            ('const value: Int = 1\nfn attempt(): Int { value() }', 'LYCT0001', '不是函数'),
        ]
        for source, code, message in cases:
            with self.subTest(source=source):
                self.write('app/src/main.ly', source + '\nfn main(): Unit {}')
                self.diagnose(self.app, code, message)

    def test_overflow_and_budgets_are_build_time_errors(self):
        cases = [
            ('const overflow: Int8 = 127 + 1', 'LYCT0003', 'Int8'),
            ('const overflow: UInt = 0 - 1', 'LYCT0003', 'UInt'),
            ('const zero: Int = 1 / 0', 'LYCT0003', '除零'),
            ('const fn bad(): Int { bad() }\nconst value: Int = bad()', 'LYCT0004', '深度'),
            ('const fn bad(): Unit { while true {} }\nconst badLoop: Unit = bad()', 'LYCT0004', '步数'),
        ]
        for source, code, message in cases:
            with self.subTest(source=source):
                self.write('app/src/main.ly', source + '\nfn main(): Unit {}')
                self.diagnose(self.app, code, message)

    def test_const_functions_validate_transitive_calls_and_evaluate_match(self):
        self.write('app/lyra.json5', '{name:"app",version:"0.1.0"}')
        self.write('app/src/main.ly', '''
const fn classify(value: Int): Int {
    match value {
        -2..<0 => 11,
        0 | 2 => 22,
        _ => 33
    }
}
const result: Int = classify(-1) + classify(2) + classify(8)
fn main(): Unit { print(result) }
''')
        result = self.cli('run', self.app)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ['66'])

        self.write('app/src/main.ly', '''
fn twice(value: Int): Int { value * 2 }
const result: Int = twice(12)
fn main(): Unit { print(result) }
''')
        result = self.cli('run', self.app)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ['24'])

        self.write('app/src/main.ly', '''
fn printLater(): Int { print("not constant"); 0 }
const fn hiddenEffect(): Int { if true { 7 } else { printLater() } }
const fn caller(): Int { hiddenEffect() }
const result: Int = caller()
fn main(): Unit {}
''')
        self.diagnose(self.app, 'LYCT0001', 'print')

    def test_const_functions_support_generic_instances_and_default_arguments(self):
        self.write('app/lyra.json5', '{name:"app",version:"0.1.0"}')
        self.write('app/src/main.ly', '''
const fn identity<Item>(value: Item): Item { value }
const fn add(value: Int, increment: Int = 3): Int { value + increment }
const result: Int = identity<Int>(5) + add(6)
fn main(): Unit { print(result) }
''')
        result = self.cli('run', self.app)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ['14'])

    def test_const_functions_evaluate_struct_enum_and_sum_values(self):
        self.write('app/lyra.json5', '{name:"app",version:"0.1.0"}')
        self.write('app/src/main.ly', '''
struct Point(val x: Int, val y: Int)
enum Choice { Count(Int), Alias(Int), Empty }
extension PointMetrics for Point {
    val total: Int get() { this.x + this.y }
}
const fn shifted(point: Point): Point { Point(point.x + 1, point.y + 2) }
const fn classify(choice: Choice): Int {
    match choice {
        Choice.Count(value) | Choice.Alias(value) if value > 0 => value,
        Choice.Count(_) | Choice.Alias(_) => 0,
        Choice.Empty => -1
    }
}
const fn maybe(value: Int): Int? {
    if value > 0 { Some(value) } else { None }
}
const fn lifted(): Int? { 7 }
const fn read(): Result<Int,String> { Ok(8) }
const fn readAndAdd(): Result<Int,String> { Ok(read()? + 1) }
const fn pointScore(): Int { val point = shifted(Point(2, 3)); point.total }
const fn unicodeChar(): Char { "a🙂"[1] }
const result: Int = pointScore() + classify(Choice.Alias(5)) + classify(Choice.Count(-1)) + classify(Choice.Empty) + (maybe(7) ?? 0) + (lifted() ?? 0) + (match readAndAdd() { Ok(value) => value, Err(_) => 0 })
fn main(): Unit { print(result) }
''')
        result = self.cli('run', self.app)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ['35'])

        self.write('app/src/main.ly', '''
const fn unicodeChar(): Char { "a🙂"[1] }
fn main(): Unit { print(unicodeChar()) }
''')
        result = self.cli('run', self.app)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ['🙂'])

    def test_const_index_out_of_bounds_is_a_build_time_diagnostic(self):
        cases = [
            ('const fn bad(): Char { "a"[1] }\nconst value: Char = bad()', 'LYCT0003', '索引越界'),
            ('const fn bad(): Char { "a"[-1] }\nconst value: Char = bad()', 'LYCT0003', '索引越界'),
            ('const fn bad(): UInt8 { b"a"[1] }\nconst value: UInt8 = bad()', 'LYCT0003', '索引越界'),
            ('const fn bad(): UInt8 { b"a"[-1] }\nconst value: UInt8 = bad()', 'LYCT0003', '索引越界'),
            ('fn bad(): Int { val numbers = [1]; numbers[1] }\nconst value: Int = bad()', 'LYCT0003', '索引越界'),
            ('fn bad(): Unit { val numbers = [1]; numbers[-1] = 2 }\nconst value: Unit = bad()', 'LYCT0003', '索引越界'),
        ]
        for source, code, message in cases:
            with self.subTest(source=source):
                self.write('app/src/main.ly', source + '\nfn main(): Unit {}')
                self.diagnose(self.app, code, message)

    def test_pure_list_algorithm_exports_a_composite_constant(self):
        self.write('app/lyra.json5', '{name:"app",version:"0.1.0",dependencies:{cfg:{name:"cfg",version:"^0.1.0",path:"../cfg"}}}')
        self.write('cfg/lyra.json5', '{name:"cfg",version:"0.1.0",kind:"lib"}')
        self.write('cfg/src/data.ly', '''pub struct Item(pub var amount: Int, pub val tag: String)
pub enum Summary { Found(Item), Missing }
fn change(values: List<Item>): Int {
    values[0] = Item(10, "updated")
    2
}
pub fn compute(): Result<Summary,String> {
    val values = [Item(1, "first"), Item(3, "second")]
    val alias = values
    if alias != values || values == [Item(1, "first"), Item(3, "second")] { return Err("identity") }
    var copy = values[0]
    copy.amount += 20
    if values[0].amount != 1 { return Err("copy") }
    alias[0].amount += change(values)
    var total = 0
    for item in alias {
        total += item.amount
        alias.add(Item(100, "extra"))
    }
    for char in "a🙂" { total += 1 }
    for byte in b"xy" { total += 1 }
    val count: Int? = Some(alias.size)
    if count == None { return Err("empty") }
    Ok(Summary.Found(Item(total + count, values[0].tag)))
}
pub const summary: Result<Summary,String> = compute()
''')
        self.write('app/src/main.ly', '''import cfg.{Item}
import cfg.{Summary}
import cfg.{compute}
import cfg.{summary}
const saved: Result<Summary,String> = summary
fn show(result: Result<Summary,String>): Unit {
    match result {
        Ok(Summary.Found(item)) => { print(item.amount); print(item.tag) },
        Ok(Summary.Missing) => { print("missing") },
        Err(error) => { print(error) }
    }
}
fn main(): Unit { show(saved); show(compute()) }
''')
        for mode in ([], ['--release']):
            with self.subTest(mode=mode):
                result = self.cli('run', self.app, *mode)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.splitlines(), ['14', 'updated', '14', 'updated'])

    def test_temporary_classes_preserve_identity_through_init_and_nested_assignments(self):
        self.write('app/src/main.ly', '''
class Counter(var value: Int = 2, var next: Counter? = None) {
    fn bump(by: Int = value): Int { value += by; value }
}
struct Point(var amount: Int)
class Box {
    val counter: Counter = Counter()
    var point: Point = Point(3)
    val name: String
    init(name: String, early: Bool = false) {
        this.name = name
        if early { return }
        point.amount += counter.bump()
    }
}
struct View(val box: Box)
fn get(boxes: List<Box>, calls: Counter): Box { calls.bump(1); boxes[0] }
fn reassign(boxes: List<Box>, second: Box): Int {
    boxes[0] = second
    second.point.amount = 40
    2
}
fn compute(): String {
    val first = Box("first")
    val second = Box("second", true)
    val view = View(first)
    val boxes = [first]
    val calls = Counter(0)
    get(boxes, calls).point.amount += reassign(boxes, second)
    view.box.counter.next = Some(first.counter)
    val cycle = match first.counter.next { Some(link) => link == first.counter, None => false }
    var copy = first.point
    copy.amount += 100
    "${first.name}:${first.point.amount}:${second.name}:${second.point.amount}:${calls.value}:${view.box == first}:${boxes[0] == first}:${first.counter != second.counter}:${cycle}:${copy.amount}"
}
const saved: String = compute()
fn main(): Unit { print(saved); print(compute()) }
''')
        expected = 'first:9:second:40:1:true:false:true:true:109'
        for mode in ([], ['--release']):
            with self.subTest(mode=mode):
                result = self.cli('run', self.app, *mode)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.splitlines(), [expected, expected])

    def test_temporary_closures_share_captures_and_get_fresh_loop_bindings(self):
        self.write('app/src/main.ly', '''
class Operations(val bump: (Int) -> Int, val read: () -> Int)
enum NumberPair { Values(Int, Int) }
fn make(start: Int): Operations {
    var count = start
    val bump = (amount: Int) => { count += amount; count }
    Operations(bump, () => { count })
}
fn apply<Item>(value: Item, action: (Item) -> Item): Item { action(value) }
fn identity<Item>(value: Item): Item { value }
fn optionalCall(action: (() -> Int)?): Int {
    if action == None { return 0 }
    action()
}
fn decline(saved: List<() -> Int>, action: () -> Int): Bool { saved.add(action); false }
fn failed(): Result<Int,String> { Err("bad") }
fn compute(): String {
    val operations = make(1)
    val first = operations.bump(2)
    val second = operations.bump(5)
    val callbacks: List<() -> Int> = []
    for number in [2, 4] {
        var current = number
        callbacks.add { current += number; current }
    }
    for number in [7, 9] {
        match NumberPair.Values(number, 0) {
            NumberPair.Values(0, bound) | NumberPair.Values(bound, 0) if decline(callbacks, () => { bound }) => {},
            _ => {}
        }
    }
    val generic: (Int) -> Int = identity
    val finish = (number: Int) => { () => { number + operations.read() } }
    val report = (): Result<Int,String> => { Ok(failed()?) }
    val result = match report() { Ok(number) => number, Err(_) => 42 }
    val counts = "${first}:${second}:${operations.read()}:${callbacks[0]()}-${callbacks[0]()}-${callbacks[1]()}-${callbacks[2]()}-${callbacks[3]()}"
    "${counts}:${apply(6, generic)}:${apply(4) { it + 1 }}:${finish(3)()}:${optionalCall(Some(() => { 10 }))}:${result}"
}
const saved: String = compute()
fn main(): Unit { print(saved); print(compute()) }
''')
        expected = '3:8:8:4-6-8-7-9:6:5:11:10:42'
        for mode in ([], ['--release']):
            with self.subTest(mode=mode):
                result = self.cli('run', self.app, *mode)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.splitlines(), [expected, expected])


if __name__ == '__main__':
    unittest.main()
