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

    def test_async_expressions_are_cold_and_preserve_scope(self):
        self.run_source('''fn number(value: Int): Int { print(value); value }
fn make(value: Int): Task<Int> {
    async { defer { print("done") }; await sleepMs(0); value + 1 }
}
fn create(increment: Int): (Int) -> Task<Int> {
    async (value) => { await sleepMs(0); return value + increment }
}
fn choose(value: Int?): Task<Int?> {
    async { val item = value?; Some(item + 1) }
}
async fn main(): Unit {
    var current = 1
    val captured = async { current }
    current = 8
    val unused = async { print("must not run") }
    val first = make(number(10))
    val callback = create(12)
    val second = callback(number(20))
    val third = (async (value: Int): Int => { await sleepMs(0); value * 2 })(7)
    print("constructed")
    print(await first)
    print(await first)
    print(await second)
    print(await third)
    print(await captured)
    print(await choose(None) == None)
    print((await choose(Some(5))) ?? -1)
    val boom = async (): Int => { throw Exception("callback failed") }
    try { await boom(); } catch error: Exception { print(error.message) }
}''', '10\n20\nconstructed\ndone\n11\n11\n32\n14\n8\ntrue\n6\ncallback failed\n')

    def test_sync_lambda_in_async_body_still_rejects_await(self):
        self.main.write_text('async fn main(): Unit { val sync = () => { await sleepMs(0) } }', encoding='utf-8')
        result = subprocess.run([COMPILER, 'check', self.package, '--diagnostics=json'], capture_output=True, timeout=10)
        error, = json.loads(result.stderr)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(error['code'], 'LYT0008', error)

    def test_channel_cold_tasks_cross_domains_and_preserve_nested_option(self):
        self.run_source('''class Entry(val number: Int)
async fn publish<T>(values: Channel<T>, item: T): Unit {
    await values.send(item)
    values.close()
}
async fn main(): Unit {
    val values: Channel<Entry?> = Channel(capacity: 1)
    val cold = values.send(None)
    val timed = await select { values.receive() => "early", sleepMs(0) => "cold" }
    print(timed)
    await cold
    await cold
    print(await values.receive() == Some(None))
    val worker = parallel { await publish(values, Some(Entry(81))); }
    val result = await select {
        val item = values.receive() => match item { Some(Some(entry)) => entry.number, _ => -1 },
        sleepMs(5000) => -2,
    }
    print(result)
    await worker
    print(await values.receive() == None)
    try { await values.send(None) } catch closed: ChannelClosed { print(closed.message) }
    try { Channel<Int>(-1); } catch reason: Exception { print("invalid capacity") }
}''', 'cold\ntrue\n81\ntrue\nsend on closed channel\ninvalid capacity\n')

    def test_channel_try_operations_distinguish_temporarily_unavailable_and_closed(self):
        self.run_source('''
fn label(result: ChannelReceiveResult<Int?>): String {
    match result {
        ChannelReceiveResult.Received(None) => "received empty element",
        ChannelReceiveResult.Received(Some(value)) => value.toString(),
        ChannelReceiveResult.Empty => "empty channel",
        ChannelReceiveResult.Closed => "closed channel",
    }
}

fn main(): Unit {
    val channel = Channel<Int?>(2)
    print(label(channel.tryReceive()))
    print(channel.trySend(None) == ChannelSendResult.Sent)
    print(channel.trySend(Some(42)) == ChannelSendResult.Sent)
    print(channel.trySend(Some(9)) == ChannelSendResult.Full)
    channel.close()
    print(channel.trySend(Some(9)) == ChannelSendResult.Closed)
    print(label(channel.tryReceive()))
    print(label(channel.tryReceive()))
    print(label(channel.tryReceive()))

    val direct = Channel<Int>(0)
    print(direct.trySend(0) == ChannelSendResult.Full)
    print(direct.tryReceive() == ChannelReceiveResult.Empty)
}
''', 'empty channel\ntrue\ntrue\ntrue\ntrue\nreceived empty element\n42\nclosed channel\ntrue\ntrue\n')

    def test_for_await_uses_the_protocol_and_does_not_close_channels(self):
        self.run_source('''class Cursor(var index: Int) {
    async fn next(): Int? {
        await sleepMs(0)
        index += 1
        if index <= 3 { Some(index) } else { None }
    }
}
struct Sequence {
    fn asyncIterator(): Cursor { print("open"); Cursor(0) }
}
async fn main(): Unit {
    for await value in Sequence() {
        if value == 2 { continue }
        print(value)
    }
    val channel = Channel<Int?>(0)
    spawn async {
        await channel.send(None)
        await channel.send(Some(42))
        channel.close()
    }
    for await item in channel { print(item == None); break }
    for await item in channel { print(item ?? -1) }
    print(await channel.receive() == None)
}''', 'open\n1\n3\ntrue\n42\ntrue\n')

    def test_await_outcome_observes_failure_and_cancelled_cleanup_snapshots(self):
        self.run_source('''async fn work(started: Channel<Int>, gate: Channel<Int>): Int {
    defer { throw Exception("second cleanup") }
    defer { throw Exception("first cleanup") }
    await started.send(1)
    (await gate.receive()) ?? -1
}
fn show(outcome: TaskOutcome<Int>): Unit {
    match outcome {
        Completed(value) => print(value),
        Failed(exception) => print(exception.message),
        Cancelled(cleanups) => {
            print(cleanups.size)
            for error in cleanups { print(error.message) }
            if cleanups.size > 0 { cleanups[0] = Exception("local change") }
        },
    }
}
async fn main(): Unit {
    show(awaitOutcome(async { 42 }))
    val failed: Task<Int> = async { throw Exception("original") }
    show(awaitOutcome(failed))
    show(awaitOutcome(failed))
    val started = Channel<Int>(0)
    val gate = Channel<Int>(0)
    val child = spawn work(started, gate)
    await started.receive();
    child.cancel()
    show(awaitOutcome(child))
    show(awaitOutcome(child))
}''', '42\noriginal\noriginal\n2\nfirst cleanup\nsecond cleanup\n2\nfirst cleanup\nsecond cleanup\n')

    def test_self_wait_is_an_exception_not_a_task_outcome(self):
        self.run_source('''async fn main(): Unit {
    var work = async {}
    work = async {
        defer print("cleanup")
        try { await work } catch error: TaskCycleError { print(error.message) }
        try { awaitOutcome(work); } catch error: TaskCycleError { print(error.message) }
        print("still running")
    }
    await work
    print("completed")
}
''', 'await would create a task wait cycle\nawaitOutcome would create a task wait cycle\nstill running\ncleanup\ncompleted\n')

    def test_mutual_await_in_one_domain(self):
        self.run_source('''async fn main(): Unit {
    var first = async {}
    var second = async {}
    first = async {
        defer print("first cleanup")
        await second
    }
    second = async {
        defer print("second cleanup")
        try { await first } catch error: TaskCycleError { print(error.message) }
    }
    await first
}
''', 'await would create a task wait cycle\nsecond cleanup\nfirst cleanup\n')

    def test_wait_cycle_across_parallel_domains(self):
        self.run_source('''async fn main(): Unit {
    var first = async {}
    var second = async {}
    val ready = Channel<Unit>(2)
    val start = Channel<Unit>(2)
    val caught = Channel<String>(2)
    val marks = [0, 0]
    first = parallel {
        defer { marks[0] = 1 }
        await ready.send({})
        await start.receive()
        try { await second } catch error: TaskCycleError { await caught.send(error.message) }
    }
    second = parallel {
        defer { marks[1] = 1 }
        await ready.send({})
        await start.receive()
        try { await first } catch error: TaskCycleError { await caught.send(error.message) }
    }
    await ready.receive()
    await ready.receive()
    await start.send({})
    await start.send({})
    await first
    await second
    caught.close()
    print(match await caught.receive() { Some(message) => message, None => "missing cycle" })
    print(await caught.receive() == None)
    print(marks[0] + marks[1])
}
''', 'await would create a task wait cycle\ntrue\n2\n', env={**os.environ, "LYRA_PARALLEL_WORKERS": "2"})

    def test_implicit_join_cycle_cancels_and_unwinds_children(self):
        self.run_source('''async fn main(): Unit {
    var owner = async {}
    owner = async {
        defer print("owner cleanup")
        spawn async {
            defer { print("child cleanup"); throw Exception("child close failed") }
            await owner
        };
        await sleepMs(0)
    }
    try { await owner } catch error: TaskCycleError {
        print(error.message)
        print(error.cleanupFailures[0].message)
    }
}
''', 'child cleanup\nowner cleanup\njoining child tasks would create a task wait cycle\nchild close failed\n')

    def test_await_detects_existing_implicit_join_edge(self):
        self.run_source('''async fn main(): Unit {
    var owner = async {}
    owner = async {
        defer print("owner cleanup")
        spawn async {
            defer print("child cleanup")
            try { await owner } catch error: TaskCycleError { print(error.message) }
        };
    }
    await owner
}
''', 'await would create a task wait cycle\nchild cleanup\nowner cleanup\n')

    def test_async_iteration_and_outcome_require_an_async_context(self):
        for program, name in [
            ('fn main(): Unit { for await value in Channel<Int>(1) {} }', 'await'),
            ('fn main(): Unit { val outcome = awaitOutcome(async { 1 }) }', 'awaitOutcome'),
        ]:
            with self.subTest(program=program):
                self.main.write_text(program, encoding='utf-8')
                result = subprocess.run([COMPILER, 'check', self.package, '--diagnostics=json'], capture_output=True, timeout=10)
                error, = json.loads(result.stderr)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(error['code'], 'LYT0008', error)
                span = error['range']
                self.assertEqual(program[span['start']['offset']:span['end']['offset']], name)

    def test_select_commits_only_the_chosen_arm_in_source_order(self):
        self.run_source('''fn mark(value: Int): Int { print(value); value }
async fn main(): Unit {
    val values = Channel<Int>(1)
    val where = await select {
        values.send(mark(17)) => "first",
        values.send(mark(32)) => "second",
        sleepMs(0) => "timer",
    }
    print(where)
    print((await values.receive()) ?? -1)
    print(await select { values.receive() => 0, sleepMs(2) => 1 })
    values.close()
    print(await select { sleepMs(0) => "expired", values.send(9) => "invalid" })
    try {
        await select { values.send(9) => print("sent"), sleepMs(0) => print("wrong timer") }
    } catch problem: ChannelClosed { print("closed arm") }
}''', '17\n32\nfirst\n17\n1\nexpired\nclosed arm\n')

    def test_select_rejects_arbitrary_tasks_and_arm_bindings_do_not_escape(self):
        cases = (
            ('fn main(): Unit { await select { sleepMs(0) => 0 }; }', 'LYT0008', 'await select'),
            ('async fn other(): Unit {}\nasync fn main(): Unit { await select { other() => 0 }; }', 'LYT0008', 'other() =>'),
            ('async fn main(): Unit { await select { val timer = sleepMs(0) => {}, sleepMs(0) => { print(timer) } } }', 'LYN0001', 'timer) }'),
        )
        for source, code, span in cases:
            with self.subTest(source=source):
                self.main.write_bytes(source.encode('utf-8'))
                checked = subprocess.run([COMPILER, 'check', self.package, '--diagnostics=json'], capture_output=True, timeout=10)
                self.assertNotEqual(checked.returncode, 0)
                error, = json.loads(checked.stderr)
                self.assertEqual((error['code'], error['range']['start']['offset']), (code, source.index(span)), error)

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
import std.sync.{AtomicInt}
import std.sync.{Mutex}
import std.sync.{withLock}

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
import std.collections.{parallelFor}
import std.sync.{AtomicInt}
import std.task.{parallelism}

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
import std.collections.{parallelFor}
import std.sync.{AtomicInt}

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
import std.collections.{parallelFor}
import std.sync.{AtomicInt}

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
import std.collections.{parallelFor}
import std.sync.{AtomicInt}

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

    def test_cpu_loop_cooperatively_checks_cancellation(self):
        (self.package / "lyra.json5").write_text("{name:'test/cooperative-cancel',version:'1.0.0'}")
        self.run_source('''
import std.sync.{AtomicInt}

class State(val started: AtomicInt, val caught: AtomicInt, val cleaned: AtomicInt)
fn spin(state: State): Unit {
    defer { state.cleaned.fetchAdd(1); throw Exception("inner") }
    state.started.store(1)
    while true { continue }
}
fn middle(state: State): Unit {
    defer { state.cleaned.fetchAdd(1); throw Exception("outer") }
    try { spin(state) } catch { state.caught.store(1) }
}
async fn cancelAndReport(job: Task<Unit>, state: State): Unit {
    while state.started.load() == 0 { await sleepMs(1) }
    job.cancel()
    match awaitOutcome(job) {
        Completed(_) => print("completed"),
        Failed(_) => print("caught as exception"),
        Cancelled(failures) => {
            print(failures.size)
            for failure in failures { print(failure.message) }
        }
    }
    print(state.caught.load())
    print(state.cleaned.load())
}
async fn main(): Unit {
    val parallelState = State(AtomicInt(), AtomicInt(), AtomicInt())
    val job = parallel { middle(parallelState) }
    await cancelAndReport(job, parallelState)
    val blockingState = State(AtomicInt(), AtomicInt(), AtomicInt())
    val work = spawn blocking { middle(blockingState) }
    await cancelAndReport(work, blockingState)
}
''', '2\ninner\nouter\n0\n2\n' * 2, env={**os.environ, "LYRA_PARALLEL_WORKERS": "1", "UV_THREADPOOL_SIZE": "1"})

    def test_locked_update_commits_before_observing_cancellation(self):
        (self.package / "lyra.json5").write_text("{name:'test/locked-value',version:'1.0.0'}")
        self.run_source('''
import std.sync.{AtomicInt, Locked}

async fn main(): Unit {
    val counter = Locked(7)
    try { counter.update { throw Exception("no change") } }
    catch { print(counter.snapshot()) }
    val ready = AtomicInt()
    val after = AtomicInt()
    val job = parallel {
        counter.update { previous =>
            ready.store(1)
            while !Task.cancelled() {}
            previous + 1
        }
        after.store(1)
    }
    while ready.load() == 0 { await sleepMs(1) }
    job.cancel()
    match awaitOutcome(job) {
        Cancelled(_) => print(counter.snapshot()),
        _ => print("not cancelled")
    }
    print(after.load())
}
''', '7\n8\n0\n', env={**os.environ, "LYRA_PARALLEL_WORKERS": "1"})

    def test_cancellation_check_observes_deadline_without_await(self):
        (self.package / "lyra.json5").write_text("{name:'test/cpu-deadline',version:'1.0.0'}")
        self.run_source('''
import std.task.{withTimeout}

fn countForever(): Unit { for value in 0..9223372036854775807 { continue } }
async fn spin(): Unit { countForever() }
async fn main(): Unit {
    taskGroup { spawn withTimeout(50, spin()); }
    print("finished")
}
''', 'finished\n')

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
import std.io.{readLine}
import std.sqlite.{DbError}
import std.sqlite.{open}

fn readDatabase(): Result<String?, DbError> {
    val db = open(":memory:")?
    defer db.close()
    val query = db.prepare("SELECT 'after close'")?
    defer query.close()
    #assert(query.next()?, "read")
    query.readText(0)
}

async fn release(): Unit { print("domain still running") }
async fn main(): Unit {
    val reading = spawn blocking {
        match readLine() { Ok(Some(value)) => value, _ => throw Exception("expected a line") }
    }
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
