import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from platform_cli import executable


COMPILER = Path(sys.argv.pop(1)).resolve()


class Exceptions(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.package = Path(directory.name)
        (self.package / 'src').mkdir()
        (self.package / 'lyra.json5').write_text('{name:"example/errors", version:"1.0.0"}')
        self.source = self.package / 'src/main.ly'

    def executable(self, source, stdout, errors=()):
        self.source.write_text(source, encoding='utf-8')
        for mode in ([], ['--release']):
            with self.subTest(mode=mode):
                app = executable(self.package / 'app')
                build = subprocess.run([COMPILER, 'build', self.package, *mode, '-o', app], capture_output=True, text=True, timeout=30)
                self.assertEqual(build.returncode, 0, build.stderr)
                result = subprocess.run([app], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.stdout.splitlines(), stdout)
                self.assertEqual(result.returncode != 0, bool(errors), result.stderr)
                if not errors:
                    self.assertEqual(result.stderr, '')
                cursor = 0
                for text in errors:
                    found = result.stderr.find(text, cursor)
                    self.assertNotEqual(found, -1, result.stderr)
                    cursor = found + len(text)

    def test_nested_calls_catch_expression_rethrow_and_result(self):
        self.executable('''fn leaf(): Int { throw Exception("boom") }
fn relay<T>(value: T): Int { leaf() }
fn catchAndRethrow(): Int { try { relay(8) } catch { gcCollect(); throw } }
fn returned(): Result<Int,String> { Err("normal value") }
fn main(): Unit {
    val value = try { catchAndRethrow() } catch error: Exception { print(error.message); 42 }
    print(value)
    val result = try { returned() } catch { Ok(9) }
    print(match result { Err(text) => text, Ok(_) => "wrong" })
    print(try { 73 } catch { 4 })
    val original = Exception(message: "same text")
    try { throw original } catch error: Exception {
        if error == original && error != Exception("same text") { print("same exception") }
    }
}''', ['boom', '42', 'normal value', '73', 'same exception'])

    def test_unhandled_rethrow_reports_original_location_and_call_chain(self):
        self.executable('''fn original(): Unit {
    throw Exception("original error")
}
fn again(): Unit { try { original() } catch { throw } }
fn main(): Unit { again(); print(99) }
''', [], ['Exception: original error', 'throw at ', 'main.ly:2:5', 'call at ', 'main.ly:4:', 'call at ', 'main.ly:5:'])

    def test_exception_during_default_or_implicit_stops_call(self):
        self.executable('''struct Bad()
struct Good()
implicit (value: Bad): Good { throw Exception("conversion") }
fn mark(): Int { print("extra"); 1 }
fn missing(): Int { throw Exception("default") }
fn call(a: Int = missing(), b: Int = mark()): Unit { print("body") }
fn take(g: Good): Unit { print("taken") }
fn failing(): Result<Int,Bad> { Err(Bad()) }
fn propagating(): Result<Int,Good> { Ok(failing()?) }
fn main(): Unit {
    try { call() } catch error: Exception { print(error.message) }
    try { take(Bad()) } catch error: Exception { print(error.message) }
    try { propagating(); } catch error: Exception { print(error.message) }
}''', ['default', 'conversion', 'conversion'])

    def test_result_error_preserves_concrete_causes_through_conversion(self):
        self.executable('''struct LookupFailed(val code: Int) {
    fn message(): String { "lookup " + code.toString() }
}
struct ServiceFailure(val code: Int) {
    fn message(): String { "service " + code.toString() }
}
implicit (failure: LookupFailed): ServiceFailure { print("convert"); ServiceFailure(failure.code) }
fn query(fail: Bool): Result<Int,LookupFailed> { if fail { Err(LookupFailed(7)) } else { Ok(12) } }
fn convert(fail: Bool): Result<Int,ServiceFailure> { Ok(query(fail)?) }
fn lift(): Result<Int,Error> { Ok(convert(true)?) }
fn forward(): Result<Int,Error> { Ok(lift()?) }
fn main(): Unit {
    match convert(false) { Ok(value) => print(value), Err(_) => print("unexpected") }
    val result = forward()
    gcCollect()
    match result.failureInfo() {
        Some(error) => {
            print(error.message)
            print(error.original is ServiceFailure)
            match error.cause {
                Some(previous) => match previous.original as? LookupFailed {
                    Some(database) => print(database.code), None => print("lost database error"),
                },
                None => print("lost cause"),
            }
        },
        None => print("unexpected success"),
    }
    val ok: Result<Int,LookupFailed> = Ok(1)
    print(ok.failureInfo() == None)
}''', ['12', 'convert', 'service 7', 'true', '7', 'true'])

    def test_common_error_wait_and_root_report_do_not_throw(self):
        self.executable('''async fn read(): Result<Int,Error> {
    val original = Error("missing table", cause: Error("database"))
    val failed = original.withSuppressed(Error("finalize")).withSuppressed(Error("close"))
    val copy = failed.suppressed
    copy[0] = Error("changed")
    print(original.suppressed.size)
    print(failed.suppressed[0].message)
    Err(failed)
}
async fn main(): Result<Unit,Error> {
    try { val value = (await read())?; print(value) } catch { print("unexpected catch") }
    Ok({})
}''', ['0', 'finalize'], ['Error: missing table', 'caused by: database', 'suppressed: finalize', 'suppressed: close'])

    def test_return_cleanup_can_be_caught_before_enclosing_scope_exits(self):
        self.executable('''fn choose(): Int {
    defer print("outer")
    try {
        defer print("inner")
        defer { throw Exception("cleanup") }
        return 1
    } catch problem: Exception { print(problem.message) }
    print("after catch")
    2
}
fn main(): Unit { print(choose()) }
''', ['inner', 'cleanup', 'after catch', 'outer', '2'])

    def test_cleanup_preserves_original_and_all_additional_causes(self):
        self.executable('''fn cleanup(message: String): Unit { print(message); throw Exception(message) }
fn main(): Unit {
    defer cleanup("first registered")
    defer cleanup("last registered")
    throw Exception("primary")
}''', ['last registered', 'first registered'], ['Exception: primary', 'main.ly:5:5', 'suppressed Exception: last registered', 'suppressed Exception: first registered'])

    def test_await_handles_failure_without_recancelling_the_group(self):
        self.executable('''async fn bad(): Int { print("once"); throw Exception("child") }
async fn nested(): Int { await bad() }
async fn later(): Unit { print("sibling") }
async fn main(): Unit {
    val child = nested()
    try { await child; } catch problem: Exception { print(problem.message) }
    try { await child; } catch problem: Exception { print(problem.message) }
    await later()
    val worker: Task<Int> = blocking { defer print("worker cleanup"); throw Exception("worker") }
    try { await worker; } catch problem: Exception { print(problem.message) }
}''', ['once', 'child', 'child', 'sibling', 'worker cleanup', 'worker'])

    def test_checked_operations_unwind_and_leave_the_program_running(self):
        self.executable(r'''fn divide(divisor: Int): Int { 1 / divisor }
async fn outOfBounds(): Int {
    defer print("async cleanup")
    val frozen = FrozenList([1])
    frozen[-1]
}
async fn main(): Unit {
    try { defer print("local cleanup"); divide(0); }
    catch error: ArithmeticError { print(error.message) }
    try { await outOfBounds(); } catch error: BoundsError { print(error.message) }
    try {
        await blocking { defer print("worker cleanup"); b"\x00"[1] };
    } catch error: BoundsError { print(error.message) }
    val values = [10, 20]
    try { values[1] = values.removeAt(0) } catch error: BoundsError { print(error.message) }
    print(values[0])
    try { values.removeAt(2); } catch error: BoundsError { print(error.message) }
    try { values.slice(0..1); } catch error: BoundsError { print(error.message) }
    try { "猫"[1]; } catch error: BoundsError { print(error.message) }
    print(b"\x00"[0])
    print("\u{0}"[0].codePoint)
}''', ['local cleanup', 'Int division by zero', 'async cleanup', 'list index -1 out of bounds (size 1)',
       'worker cleanup', 'Bytes index 1 out of bounds (size 1)', 'list index 1 out of bounds (size 1)', '20',
       'list index 2 out of bounds (size 1)', 'list slice 0..1 out of bounds (size 1)',
       'String index 1 out of bounds (size 1)', '0', '0'])

    def test_checked_cleanup_failure_is_attached_after_the_primary_error(self):
        self.executable('''fn main(): Unit {
    defer { [1][1]; }
    val result = 1 << 64
}''', [], ['ArithmeticError: shift count out of range', 'main.ly:3:',
           'suppressed BoundsError: list index 1 out of bounds (size 1)', 'main.ly:2:'])

    def test_unobserved_spawn_and_await_sites_reach_entry(self):
        self.executable('''async fn bad(): Unit { throw Exception("child") }
async fn group(): Unit { await bad() }
async fn main(): Unit { spawn group(); }
''', [], ['Exception: child', 'throw at ', 'main.ly:1:', 'main.ly:2:', 'await at ', 'main.ly:2:', 'main.ly:3:'])

    def test_rethrow_after_suspension_preserves_the_observed_exception(self):
        self.executable('''async fn checkpoint(): Unit { gcCollect() }
async fn original(): Int { throw Exception("from child") }
async fn relay(): Int {
    try { await original() } catch error: Exception {
        await checkpoint()
        print(error.message)
        throw
    }
}
async fn main(): Unit { print(await relay()) }
''', ['from child'], ['Exception: from child', 'main.ly:2:', 'await at ', 'main.ly:4:', 'await at ', 'main.ly:10:'])

    def test_cancel_skips_catch_and_retains_cleanup_failure(self):
        self.executable('''async fn leaf(): Unit { print("too late") }
async fn victim(): Unit {
    defer { print("cleanup"); throw Exception("failure during cancellation") }
    try { await leaf() } catch { print("caught cancellation in child") }
}
async fn cancel(child: Task<Unit>): Unit { child.cancel() }
async fn main(): Unit {
    val child = spawn victim()
    spawn cancel(child)
    try { await child } catch { print("caught cancellation in parent") }
}
''', ['cleanup'], ['cancelled', 'failure during cancellation', 'main.ly:3:'])

    def test_result_early_return_runs_cleanups_and_is_replaced_by_first_failure(self):
        self.executable('''fn read(): Result<Int,String> { Err("value") }
fn fail(message: String): Unit { print(message); throw Exception(message) }
fn call(): Result<Int,String> {
    defer fail("older cleanup")
    defer fail("newer cleanup")
    Ok(read()?)
}
fn main(): Unit { call(); }
''', ['newer cleanup', 'older cleanup'], ['Exception: newer cleanup', 'main.ly:2:', 'suppressed Exception: older cleanup'])

    def test_typed_catches_filter_in_source_order_and_keep_identity(self):
        (self.package / 'src/errors').mkdir()
        (self.package / 'src/errors/types.ly').write_text('pub exception Missing\npub exception Invalid\n')
        self.executable('''import self.errors.{Missing as MissingEntry}
import self.errors.{Invalid}
fn wrongGuard(): Bool { print("wrong guard"); true }
async fn reject(error: MissingEntry): Bool {
    await blocking { gcCollect() }
    print(error.message); false
}
async fn main(): Unit {
    val original = MissingEntry(message: "checked")
    val value = try { throw original }
    catch wrong: Invalid if wrongGuard() { 0 }
    catch problem: MissingEntry if await reject(problem) { 1 }
    catch problem: MissingEntry { if problem == original { 42 } else { 2 } }
    catch { 3 }
    print(value)
    val root: Exception = original
    try {
        try { throw root } catch wrong: Invalid {}
    } catch problem: Exception { print(problem == original) }
}''', ['checked', '42', 'true'])

    def test_typed_exception_survives_rethrow_and_suppression(self):
        self.executable('''exception Missing
exception Cleanup
fn leaf(): Unit {
    defer { throw Cleanup("secondary") }
    throw Missing("primary")
}
fn relay(): Unit { try { leaf() } catch problem: Missing if problem.message == "primary" { throw } }
fn main(): Unit { relay() }
''', [], ['Missing: primary', 'main.ly:5:5', 'suppressed Cleanup: secondary', 'main.ly:4:'])

    def test_diagnostics_reject_illegal_rethrow_and_types(self):
        cases = (
            ('fn main(): Unit { throw }', 'LYE0001', 'throw'),
            ('fn main(): Unit { throw 5 }', 'LYT0001', '5'),
            ('fn main(): Unit { try {} catch problem: Int {} }', 'LYE0002', 'Int'),
            ('fn main(): Unit { try {} catch problem: Exception if 123 {} }', 'LYT0001', '123'),
            ('fn main(): Unit { try {} catch problem: Exception {} catch {} }', 'LYE0002', 'catch {}'),
            ('fn main(): Unit { try {} catch problem: Exception {}; print(problem.message) }', 'LYN0001', 'problem.message'),
            ('fn main(): Unit { try {} catch { defer { throw } } }', 'LYE0001', 'throw'),
            ('fn main(): Unit throws Int {}', 'LYE0003', 'Int'),
            ('type Other = Exception\nfn main(): Unit throws Exception, Other {}', 'LYE0003', 'Other {}'),
            ('exception Hidden\npub fn api(): Unit throws Hidden {}\nfn main(): Unit {}', 'LYN0003', 'Hidden {}'),
        )
        for source, code, token in cases:
            with self.subTest(source=source):
                self.source.write_bytes(source.encode('utf-8'))
                result = subprocess.run([COMPILER, 'check', self.package, '--diagnostics=json'], capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                item, = json.loads(result.stderr)
                self.assertEqual((item['code'], item['range']['start']['offset']), (code, source.index(token)), item)


    def test_structured_failures_keep_the_original_exceptions_and_order(self):
        self.executable('''exception Lost
exception CloseFailed
async fn work(original: Lost, release: CloseFailed): Unit {
    defer { throw release }
    throw original
}
async fn main(): Unit {
    val original = Lost("task")
    val release = CloseFailed("child cleanup")
    val outer = CloseFailed("group cleanup")
    try {
        taskGroup {
            defer { throw outer }
            spawn work(original, release);
        }
    } catch problem: Lost {
        gcCollect()
        print(problem == original)
        print(problem.taskFailures.size)
        print(problem.taskFailures[0] == original)
        print(problem.taskFailures[0].cleanupFailures[0] == release)
        print(problem.cleanupFailures[0] == outer)
        val copy = problem.taskFailures
        copy[0] = Exception("changed")
        print(problem.taskFailures[0].message)
    }
    print(original.cleanupFailures.size)
    try {
        defer { throw CloseFailed("older") }
        defer { throw CloseFailed("newer") }
    } catch problem: CloseFailed {
        print(problem == problem.cleanupFailures[0])
        for failure in problem.cleanupFailures { print(failure.message) }
    }
}''', ['true', '1', 'true', 'true', 'true', 'task', '0', 'true', 'newer', 'older'])

    def test_throws_is_cross_module_documentation_not_an_effect(self):
        (self.package / 'src/errors').mkdir()
        (self.package / 'src/errors/types.ly').write_text('pub exception Missing\n')
        self.executable('''import self.errors.{Missing as MissingEntry}
pub fn preserve<T>(value: T): T throws MissingEntry, Exception { value }
fn throws(): Int { 7 }
fn unexpected(): Unit throws MissingEntry { throw Exception("not declared") }
fn main(): Unit {
    val callable: (Int) -> Int = preserve
    print(callable(throws()))
    try { unexpected() } catch problem: Exception { print(problem.message) }
}''', ['7', 'not declared'])


if __name__ == '__main__':
    unittest.main()
