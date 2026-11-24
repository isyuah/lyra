import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from platform_cli import executable, running


COMPILER = Path(sys.argv.pop(1)).resolve()


class Async(unittest.TestCase):
    def setUp(self):
        workspace = tempfile.TemporaryDirectory()
        self.addCleanup(workspace.cleanup)
        self.package = Path(workspace.name)
        directory = self.package / "src"
        directory.mkdir()
        self.main = directory / "main.ly"

    def run_source(self, source, expected, env=None):
        self.main.write_text(source, encoding="utf-8")
        for mode in ([], ["--release"]):
            with self.subTest(mode=mode):
                output = Path(tempfile.mkdtemp(dir=self.package, prefix="app-"))
                app = executable(output / "app")
                built = subprocess.run([COMPILER, "build", self.package, *mode, "-o", app], capture_output=True, timeout=30)
                self.assertEqual(built.returncode, 0, built.stderr.decode())
                run = subprocess.run([app], capture_output=True, timeout=15, env=env)
                self.assertEqual((run.returncode, run.stdout.decode(), run.stderr), (0, expected, b""))

    def test_cold_arguments_repeated_await_and_spawn(self):
        self.run_source("""
fn mark(value: Int): Int { print(value); value }
async fn work(value: Int): Int { print(20); value + 1 }
async fn main(): Unit {
    val unused = work(mark(8))
    val cold = work(mark(10))
    print(11)
    print(await cold)
    print(await cold)
    val running = spawn work(30)
    print(40)
    print(await running)
    spawn work(50);
}
""", "8\n10\n11\n20\n11\n11\n40\n20\n31\n20\n")

    def test_gc_in_child_and_values_live_across_await(self):
        self.run_source("""
class Cell(var number: Int)
struct Counter(var value: Int)
fn sum(first: Int, second: Int): Int { first + second }
async fn value(number: Int): Int { gcCollect(); number }
async fn make(): Cell { gcCollect(); Cell(63) }
async fn counter(): Counter { Counter(12) }
async fn main(): Unit {
    val cell = Cell(1)
    cell.number += await value(3)
    print(sum(cell.number, await value(5)))
    val held = make()
    val original = await held
    original.number = 7
    print((await held).number)
    val heldCounter = counter()
    var copy = await heldCounter
    copy.value += 1
    print(copy.value)
    print((await heldCounter).value)
    var count = 0
    while count < 3 {
        val next = if count == 1 { await value(2) } else { await value(1) }
        count += next
    }
    print(count)
}
""", "9\n7\n13\n12\n3\n")

    def test_deep_await_chain_returns_without_native_stack_recursion(self):
        self.run_source("""
async fn depth(count: Int): Int {
    if count == 0 { 0 } else { 1 + await depth(count - 1) }
}
async fn main(): Unit { print(await depth(5000)) }
""", "5000\n")

    def test_blocking_closures_capture_bindings_and_have_their_own_return(self):
        self.run_source("""
fn escaped(): Task<Int> {
    var number = 3
    val job = blocking { gcCollect(); number += 2; return number }
    number = 9
    job
}
async fn main(): Unit {
    val work = escaped()
    gcCollect()
    print(await work)
    print(await work)
    var label = "before"
    val text = blocking { label = label + "!"; label }
    label = "after"
    print(await text)
    print(label)
    var first = blocking { 0 }
    var second = blocking { 0 }
    var index = 0
    while index < 2 {
        val saved = index + 5
        val job = blocking { saved }
        if index == 0 { first = job } else { second = job }
        index += 1
    }
    print(await first)
    print(await second)
    val nested = blocking { blocking { label } }
    print(await (await nested))
}
""", "11\n11\nafter!\nafter!\n5\n6\nafter!\n")

    def test_propagate_in_blocking_exits_only_the_closure(self):
        self.run_source('''
fn absent(): Int? { None }
async fn main(): Unit {
    val pending: Task<Int?> = blocking { val value = absent()?; value + 1 }
    print(match await pending { None => 9, Some(number) => number })
    print(3)
}
''', "9\n3\n")

    def test_task_group_joins_lexically_and_keeps_errors_inside_scope(self):
        self.run_source('''
exception ChildFailed
async fn child(): Unit { print("child") }
async fn broken(): Unit { throw ChildFailed("group failure") }
fn missing(): Int? { None }
async fn main(): Unit {
    val result = taskGroup {
        defer print("cleanup")
        spawn child()
        print("body")
        return 7
    }
    print(result)
    val optional: Int? = taskGroup { missing()? + 1 }
    print(optional == None)
    try { taskGroup { spawn broken(); }; } catch reason: ChildFailed { print(reason.message) }
    print("outside")
}
''', 'body\nchild\ncleanup\n7\ntrue\ngroup failure\noutside\n')

    def test_parallel_domain_rendezvous_and_locked_gc_references(self):
        (self.package / "lyra.json5").write_text("{name:'test/domains',version:'1.0.0'}")
        self.run_source('''
import std.sync.AtomicInt
import std.sync.Mutex
import std.sync.withLock

class Counter(var value: Int)
async fn delimited(value: Int): Int { gcCollect(); value }
async fn main(): Unit {
    val gate = AtomicInt()
    val lock = Mutex()
    val counter = Counter(0)
    val items: List<Int> = []
    var captured = 0
    try { withLock(lock) { throw Exception("unlock") }; } catch { print("caught") }
    val first = parallel {
        gate.fetchAdd(1)
        while gate.load() < 2 {}
        val value = await delimited(11)
        withLock(lock) { captured += value; counter.value += 1; items.add(value) }
        value
    }
    val second = parallel {
        gate.fetchAdd(1)
        while gate.load() < 2 {}
        val value = await delimited(31)
        withLock(lock) { captured += value; counter.value += 1; items.add(value) }
        value
    }
    print((await first) + (await second))
    print(captured)
    print(counter.value)
    print(items[0] + items[1])
    print(gate.compareExchange(2, 4))
    print(gate.load())
}
''', 'caught\n42\n42\n2\n42\ntrue\n4\n')

    def test_parallel_for_respects_runtime_worker_capacity(self):
        (self.package / "lyra.json5").write_text("{name:'test/parallel-for',version:'1.0.0'}")
        source = '''
import std.collections.parallelFor
import std.sync.AtomicInt
import std.task.parallelism

class Totals(val active: AtomicInt, val maximum: AtomicInt, val sum: AtomicInt)
async fn add(totals: Totals, value: Int, expectedWorkers: Int): Unit {
    val active = totals.active.fetchAdd(1) + 1
    var previous = totals.maximum.load()
    while active > previous && !totals.maximum.compareExchange(previous, active) { previous = totals.maximum.load() }
    if value <= expectedWorkers {
        while totals.maximum.load() < expectedWorkers {}
    }
    totals.sum.fetchAdd(value)
    totals.active.fetchAdd(-1);
}
async fn main(): Unit {
    val totals = Totals(AtomicInt(), AtomicInt(), AtomicInt())
    val capacity = parallelism()
    val expectedWorkers = if capacity < 2 { capacity } else { 2 }
    await parallelFor([1, 2, 3, 4, 5], maxConcurrency: 2) { add(totals, it, expectedWorkers) }
    print(totals.sum.load())
    print(totals.maximum.load())
    print(totals.active.load())
    print(capacity)
}
'''
        self.run_source(source, '15\n2\n0\n3\n', env={**os.environ, "LYRA_PARALLEL_WORKERS": "3"})
        self.run_source(source, '15\n1\n0\n1\n', env={**os.environ, "LYRA_PARALLEL_WORKERS": "1"})

    def test_parallel_for_empty_input_and_read_only_input_contract(self):
        (self.package / "lyra.json5").write_text("{name:'test/parallel-for-input',version:'1.0.0'}")
        self.run_source('''
import std.collections.parallelFor
import std.sync.AtomicInt

class Totals(val count: AtomicInt, val sum: AtomicInt)
async fn record(totals: Totals, value: Int): Unit {
    totals.count.fetchAdd(1)
    totals.sum.fetchAdd(value);
}
async fn main(): Unit {
    val totals = Totals(AtomicInt(), AtomicInt())
    val empty: List<Int> = []
    await parallelFor(empty, maxConcurrency: 2) { record(totals, it) }
    print(totals.count.load())

    val input = [3, 5, 7, 11]
    await parallelFor(input, maxConcurrency: 2) { record(totals, it) }
    print(totals.count.load())
    print(totals.sum.load())
    print(input.size)
    print(input[0] + input[1] + input[2] + input[3])

    try { await parallelFor(empty, maxConcurrency: 0) { record(totals, it) } }
    catch error: Exception { print(error.message) }
    print(totals.count.load())
}
''', '0\n4\n26\n4\n26\nparallelFor maxConcurrency must be positive\n4\n',
                       env={**os.environ, "LYRA_PARALLEL_WORKERS": "2"})

    def test_parallel_for_failure_propagates_and_runs_cleanup(self):
        (self.package / "lyra.json5").write_text("{name:'test/parallel-for-failure',version:'1.0.0'}")
        self.run_source('''
import std.collections.parallelFor
import std.sync.AtomicInt

exception ItemFailed
class State(val cleaned: AtomicInt, val completed: AtomicInt)
async fn action(state: State, item: Int): Unit {
    defer state.cleaned.fetchAdd(1)
    if item == 1 { throw ItemFailed("item 1 failed") }
    state.completed.fetchAdd(1);
}
async fn main(): Unit {
    val state = State(AtomicInt(), AtomicInt())
    try { await parallelFor([1], maxConcurrency: 1) { action(state, it) } }
    catch error: ItemFailed { print(error.message) }
    print(state.cleaned.load())
    print(state.completed.load())
}
''', 'item 1 failed\n1\n0\n', env={**os.environ, "LYRA_PARALLEL_WORKERS": "2"})

    def test_parallel_for_cancellation_waits_for_worker_cleanup(self):
        (self.package / "lyra.json5").write_text("{name:'test/parallel-for-cancel',version:'1.0.0'}")
        source = '''
import std.collections.parallelFor
import std.sync.AtomicInt

class State(val started: AtomicInt, val release: AtomicInt, val cleaned: AtomicInt, val completed: AtomicInt)
async fn action(state: State, item: Int): Unit {
    state.started.fetchAdd(1)
    defer state.cleaned.fetchAdd(1)
    await blocking { while state.release.load() == 0 {} }
    state.completed.fetchAdd(1);
}
async fn cancelAfterStart(job: Task<Unit>, state: State): Unit {
    await blocking {
        while state.started.load() < 2 {}
    }
    job.cancel()
    state.release.store(1)
}
async fn main(): Unit {
    val state = State(AtomicInt(), AtomicInt(), AtomicInt(), AtomicInt())
    taskGroup {
        val job = spawn parallelFor([1, 2, 3, 4], maxConcurrency: 2) { action(state, it) }
        spawn cancelAfterStart(job, state)
    }
    print(state.started.load())
    print(state.cleaned.load())
    print(state.completed.load())
}
'''
        self.run_source(source, '2\n2\n0\n', env={**os.environ, "LYRA_PARALLEL_WORKERS": "2", "UV_THREADPOOL_SIZE": "3"})

    def test_typed_task_local_keeps_unset_and_stored_none_distinct(self):
        self.run_source('''
async fn inspect(key: TaskLocal<String?>): String {
    await blocking {
        gcCollect()
        match key.get() { None => "unset", Some(None) => "empty", Some(Some(value)) => value }
    }
}
async fn nested(cold: Task<String>): String {
    val work = parallel { await cold }
    await work
}
async fn main(): Unit {
    val key = TaskLocal<String?>()
    val distinct: TaskLocal<String?> = TaskLocal()
    val cold = inspect(key)
    print(key.get() == None)
    print(await withLocal(key, "request 🤝", nested(cold)))
    print(await withLocal(key, None, inspect(key)))
    print(await withLocal(distinct, "other", inspect(key)))
    print(await cold)
    print(key.get() == None)
}
''', 'true\nrequest 🤝\nempty\nunset\nrequest 🤝\ntrue\n')

    def test_business_error_main_drains_children_and_reports_the_message(self):
        for outcome, status, message in (("Ok({})", 0, b""), ('Err("数据\\0错误")', 1, "数据\0错误\n".encode())):
            self.main.write_text('async fn child(): Unit { print(1) }\n'
                                 'async fn main(): Result<Unit,String> { spawn child(); ' + outcome + ' }', encoding="utf-8")
            for mode in ([], ["--release"]):
                with self.subTest(outcome=outcome, mode=mode):
                    app = executable(self.package / "app")
                    built = subprocess.run([COMPILER, "build", self.package, *mode, "-o", app], capture_output=True, timeout=30)
                    self.assertEqual(built.returncode, 0, built.stderr.decode())
                    run = subprocess.run([app], capture_output=True, timeout=10)
                    self.assertEqual((run.returncode, run.stdout, run.stderr), (status, b"1\n", message))

    def test_blocking_io_leaves_domain_free_and_sqlite_results_survive(self):
        (self.package / "lyra.json5").write_text("{name:'test/blocking',version:'1.0.0'}")
        self.main.write_text('''
import std.io.assert
import std.io.readLine
import std.sqlite.DbError
import std.sqlite.open

fn readDatabase(): Result<String?, DbError> {
    val db = open(":memory:")?
    defer db.close()
    val query = db.prepare("SELECT 'after close'")?
    defer query.close()
    assert(query.next()?, "read")
    query.readText(0)
}

async fn release(): Unit { print("domain still running") }
async fn main(): Unit {
    val reading = spawn blocking { readLine().value }
    spawn release();
    print(await reading)
    val result = await blocking {
        val read = readDatabase()
        gcCollect()
        read
    }
    gcCollect()
    match result {
        Ok(Some(text)) => print(text),
        Ok(None) => throw Exception("expected a text result"),
        Err(error) => throw Exception(error.message()),
    }
}
''', encoding="utf-8")
        for flags in ([], ["--release"]):
            with self.subTest(flags=flags):
                app = executable(self.package / "app")
                built = subprocess.run([COMPILER, "build", self.package, "-o", app, *flags], capture_output=True, timeout=30)
                self.assertEqual(built.returncode, 0, built.stderr.decode())
                with running([app], cwd=self.package, env={**os.environ, "UV_THREADPOOL_SIZE": "1"}) as (process, output):
                    self.assertEqual(output.line(), b"domain still running\n")
                    process.stdin.write(b"ready\n")
                    process.stdin.flush()
                    self.assertEqual(process.wait(timeout=10), 0)
                    self.assertEqual((output.remaining(), process.stderr.read()), (b"ready\nafter close\n", b""))

    def test_invalid_task_operations(self):
        for source, marked in (("fn main(): Unit { val bad = await work() } async fn work(): Unit {}", "await"),
                               ("fn main(): Unit { val bad = spawn work() } async fn work(): Unit {}", "spawn"),
                               ("async fn main(): Unit { val bad = await 4 }", "4"),
                               ("async fn main(): Unit { val bad = spawn 5 }", "5"),
                               ("async fn main(): Unit { val bad = blocking { await never() } }", "await")):
            with self.subTest(source=source):
                self.main.write_text(source)
                result = subprocess.run([COMPILER, "check", self.package, "--diagnostics=json"], capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                error, = json.loads(result.stderr)
                where = error["range"]
                self.assertEqual((error["code"], where["start"]["offset"], where["end"]["offset"]),
                                 ("LYT0008", source.index(marked), source.index(marked) + len(marked)))


if __name__ == "__main__":
    unittest.main()
