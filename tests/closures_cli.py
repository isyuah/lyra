import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from platform_cli import executable


COMPILER = Path(sys.argv.pop(1)).resolve()


class Closures(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.package = Path(folder.name)
        (self.package / 'src/tools').mkdir(parents=True)
        (self.package / 'lyra.json5').write_text('{name:"example/closures",version:"1.0.0"}')
        (self.package / 'src/tools/math.ly').write_text('pub fn plusOne(value: Int): Int { value + 1 }')
        self.main = self.package / 'src/main.ly'

    def executable(self, source, lines, frames=()):
        self.main.write_text(source, encoding='utf-8')
        for mode in ([], ['--release']):
            with self.subTest(mode=mode):
                app = executable(self.package / 'app')
                built = subprocess.run([COMPILER, 'build', self.package, *mode, '-o', app], text=True, capture_output=True, timeout=30)
                self.assertEqual(built.returncode, 0, built.stderr)
                ran = subprocess.run([app], text=True, capture_output=True, timeout=15)
                self.assertEqual((ran.returncode != 0, ran.stdout.splitlines()), (bool(frames), lines), ran.stderr)
                if frames:
                    observed = [frame.strip().rsplit(':', 1)[0] for frame in ran.stderr.splitlines() if ' at ' in frame]
                    self.assertEqual(observed, [f'{role} at {self.main.as_posix()}:{line}' for role, line in frames], ran.stderr)
                else:
                    self.assertEqual(ran.stderr, '')

    def test_functions_explicit_tail_lambda_and_shared_cells(self):
        self.executable('''import self.tools.plusOne as next
class Pair(val bump: (Int) -> Int, val read: () -> Int)
fn make(start: Int): Pair {
    var running = start
    val bump = (step: Int): Int => { running += step; running }
    Pair(bump, () => { running })
}
fn visit(values: List<Int>, show: (Int) -> Unit): Unit { show(values[0]); show(values[1]) }
fn apply<T,R>(value: T, transform: (T) -> R): R { transform(value) }
fn call(action: () -> Int): Int { action() }
fn printInt(value: Int): Unit { print(value) }
fn identity<T>(value: T): T { value }
fn first<T>(values: T...): T { values[0] }
fn main(): Unit {
    val nextFn = next
    print(nextFn(5))
    val pair = make(10)
    gcCollect()
    print(pair.bump(3)); print(pair.read()); print(pair.bump(2))
    visit(values: [2, 5]) { print(next(it)) }
    visit([7, 8], printInt)
    val next = (value: Int) => { value + 100 }
    print(next(1))
    print(apply(4) { apply(it + 2) { it * 3 } })
    val generic: (Int) -> Int = identity
    print(apply(9, generic)); print(call { 23 })
    val actions: List<(Int) -> Int> = [nextFn, next]
    print(actions[0](10)); print(actions[1](10))
    print(apply(transform: (number) => { number + 1 }, value: 7))
    val select: (List<String>) -> String = first
    print(select(["first", "second"]))
    val optional: ((Int) -> Int)? = identity
    print(match optional { Some(func) => func(21), None => 0 })
}''', ['6', '13', '13', '15', '3', '6', '7', '8', '101', '18', '9', '23', '11', '110', '8', 'first', '21'])

    def test_fresh_loop_binding_and_callee_before_its_arguments(self):
        self.executable('''fn read<T>(value: T): T { value }
fn mark(value: Int): Int { print("argument"); value }
fn callback(): (Int) -> Int { print("callee"); (number) => { number * 2 } }
fn ready(): Bool { true }
fn accepts(action: () -> Bool): Bool { action() }
fn withDefault(start: Int = 2, next: (Int) -> Int): Int { next(start) }
fn main(): Unit {
    val saved: List<() -> Int> = []
    for element in [4, 8] { saved.add { element } }
    gcCollect(); print(saved[0]()); print(saved[1]())
    if ready() { print(callback()(mark(3))) }
    if (accepts { true }) { print(withDefault { it + 5 }) }
    val apply = read<((Int) -> Int) -> Int>((operation) => { operation(10) })
    print(apply { it * 2 })
    val fail = () => { throw Exception("bottom") }
    print(try { if fail() { 1 } else { 2 } } catch { 31 })
}''', ['4', '8', 'callee', 'argument', '6', '7', '20', '31'])

    def test_async_retains_mutable_capture_and_indirect_exceptions(self):
        self.executable('''class Box(val name: String)
async fn checkpoint(): Unit { gcCollect() }
async fn twice(value: Int): Int { print("started"); value * 2 }
fn factory(box: Box): () -> String { () => { box.name } }
fn fails(): Int { throw Exception("indirect") }
async fn main(): Unit {
    val getter = factory(Box("kept" + " by closure"))
    var count = 5
    val add = (delta: Int) => { count += delta; count }
    val invoke: (Int) -> Task<Int> = twice
    val cold = invoke(8)
    print("cold")
    await checkpoint()
    print(getter()); print(await cold)
    print(await blocking { add(3) }); print(count)
    val errors = [fails]
    print(try { errors[0]() } catch problem: Exception { print(problem.message); 42 })
}''', ['cold', 'kept by closure', 'started', '16', '8', '8', 'indirect', '42'])

    def test_indirect_async_factory_records_call_instead_of_reference_or_relay(self):
        self.executable('''async fn fail(): Unit { throw Exception("from indirect task") }
fn relay(task: Task<Unit>): Task<Unit> { task }
async fn main(): Unit {
    val factory = fail
    val child = factory()
    val forward = relay
    await forward(child)
}''', [], [('throw', 1), ('created', 5), ('await', 7), ('created', 3)])

    def test_return_and_propagation_end_nearest_lambda(self):
        self.executable('''fn run(operation: () -> Result<Int,String>): Result<Int,String> { operation() }
fn read(): Result<Int,String> { Err("bad") }
fn probe(fast: Bool): Int {
    val result = run {
        defer print("first")
        defer print("second")
        if fast { return Ok(3) }
        Ok(read()?)
    }
    print("after")
    match result { Ok(value) => value, Err(_) => 9 }
}
fn returner(): (Int) -> (Int) -> Int { (outer: Int) => { (inner: Int) => { outer + inner } } }
fn main(): Unit { print(probe(false)); print(probe(true)); print(returner()(2)(5)) }''',
                        ['second', 'first', 'after', '9', 'second', 'first', 'after', '3', '7'])

    def test_invalid_calls_point_to_bad_argument(self):
        cases = [
            ('fn first(show: (Int) -> Unit, values: Int...): Unit {}', 'first((value: Int) => { print(value) }, 2) { print(it) }', 'LYT0015', '{ print(it)'),
            ('fn fixed(show: (Int) -> Unit): Unit {}', 'fixed((value: Int) => { print(value) }) { print(it) }', 'LYT0002', '{ print(it)'),
            ('fn needs(f: (Int, Int) -> Int): Unit {}', 'needs { it }', 'LYT0015', '{ it'),
            ('', 'val local = 4; local()', 'LYT0015', 'local()'),
            ('', 'val local = (value) => { value }', 'LYT0015', 'value'),
            ('fn read(): Int? { None }', 'val local = () => { read()? }', 'LYT0013', '?'),
            ('async fn pause(): Unit {}', 'val local = () => { await pause() }', 'LYT0008', 'await'),
            ('fn ordinary(value: Int): Int { value }', 'ordinary = (number: Int) => { number }', 'LYT0004', 'ordinary'),
        ]
        for declaration, body, code, bad in cases:
            with self.subTest(body=body):
                program = declaration + '\nfn main(): Unit { ' + body + ' }'
                self.main.write_bytes(program.encode())
                checked = subprocess.run([COMPILER, 'check', self.package, '--diagnostics=json'], text=True, capture_output=True, timeout=10)
                self.assertNotEqual(checked.returncode, 0)
                error, = json.loads(checked.stderr)
                self.assertEqual((error['code'], error['range']['start']['offset']), (code, program.index(bad, program.index('fn main'))), error)
                if body.startswith('fixed('):
                    column = program.splitlines()[1].index('(value: Int)') + 1
                    self.assertIn(f'main.ly:2:{column}', error['hint'])


if __name__ == '__main__':
    unittest.main()
