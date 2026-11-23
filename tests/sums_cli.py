import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from platform_cli import executable


COMPILER = Path(sys.argv.pop(1)).resolve()


class Sums(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.package = Path(temporary.name)
        self.src = self.package / "src"
        self.src.mkdir()
        self.main = self.src / "main.ly"

    def native(self, source, expected):
        self.main.write_text(source, encoding="utf-8")
        for profile, options in (("debug", []), ("release", ["--release"])):
            with self.subTest(profile=profile):
                app = executable(self.package / profile)
                build = subprocess.run([COMPILER, "build", self.package, *options, "-o", app], capture_output=True, timeout=30)
                self.assertEqual(build.returncode, 0, build.stderr.decode())
                run = subprocess.run([app], capture_output=True, timeout=10)
                self.assertEqual((run.returncode, run.stdout, run.stderr), (0, expected, b""))

    def rejects(self, source, code, fragment):
        self.main.write_text(source, encoding="utf-8")
        check = subprocess.run([COMPILER, "check", self.package, "--diagnostics=json"], capture_output=True, timeout=10)
        self.assertNotEqual(check.returncode, 0)
        error, = json.loads(check.stderr)
        self.assertEqual(error["code"], code, error)
        self.assertIn(fragment, error["message"])

    def test_nested_optional_and_first_matching_arm(self):
        self.native('''
fn label(value: Option<String?>): String {
    match value {
        Some(None) => "inner",
        None => "outer",
        Some(Some(text)) => text,
        _ => "too late",
    }
}
fn main(): Unit {
    print(label(null))
    print(label(Some(null)))
    print(label(Some(Some("value"))))
}
''', b"outer\ninner\nvalue\n")

    def test_propagates_error_once_and_short_circuits(self):
        self.native('''
class Counter(var calls: Int)
class Reason(val text: String)
fn read(counter: Counter, reason: Reason, valid: Bool): Result<Int,Reason> {
    counter.calls += 1
    if valid { Ok(11) } else { Err(reason) }
}
fn convert(counter: Counter, reason: Reason, valid: Bool): Result<String,Reason> {
    val value = read(counter, reason, valid)?
    print(value)
    Ok("done")
}
fn absent(): String? { None }
fn optional(): Int? { val ignored = absent()?; print(99); Some(7) }
fn main(): Unit {
    val counter = Counter(0)
    val reason = Reason("db failure")
    print(match convert(counter, reason, false) { Ok(text) => text, Err(cause) => { if cause == reason { cause.text } else { "lost" } } })
    print(match convert(counter, reason, true) { Err(_) => "unexpected", Ok(text) => text })
    print(match optional() { Some(value) => value, None => 3 })
    print(counter.calls)
}
''', b"db failure\n11\ndone\n3\n2\n")

    def test_generic_sums_from_argument_and_expected_result(self):
        self.native('''
fn none<T>(): T? { null }
fn invert<T,E>(value: Result<T,E>): Result<E,T> {
    match value { Err(error) => Ok(error), Ok(item) => Err(item) }
}
fn main(): Unit {
    val input: Result<String,Int> = Ok("payload")
    val output = invert(input)
    print(match output { Err(text) => text, Ok(_) => "unexpected" })
    val missing: String? = none()
    print(match missing { None => 1, Some(_) => 0 })
}
''', b"payload\n1\n")

    def test_lift_preserves_payload_and_null_tests_only_the_outer_tag(self):
        self.native('''
class Label(val name: String)
struct Box(val payload: Result<Label,Int>)
fn title(name: String): String { name }
fn wrap(box: Box): Result<Label,Int>? { box.payload }
fn plain(): String? { title("from call") }
fn main(): Unit {
    val box = Box(Ok(Label("rooted")))
    val outer = wrap(box)
    gcCollect()
    print(match outer { None => "absent", Some(Err(_)) => "error", Some(Ok(label)) => label.name })
    val inner: Int? = null
    val lifted: Option<Int?> = inner
    if null != lifted { print(1) }
    if inner == null { print(2) }
    print(match plain() { Some(name) => name, None => "bad" })
}
''', b"rooted\n1\n2\nfrom call\n")

    def test_await_precedes_propagation(self):
        self.native('''
async fn read(ok: Bool): Result<Int,String> { if ok { Ok(5) } else { Err("lost") } }
async fn twice(ok: Bool): Result<Int,String> { val value = await read(ok)?; Ok(value * 2) }
async fn plain(): String { "awaited" }
async fn lift(): String? { await plain() }
async fn main(): Unit {
    print(match await twice(true) { Ok(value) => value, Err(_) => 0 })
    print(match await twice(false) { Err(message) => message, Ok(_) => "unexpected" })
    print(match await lift() { Some(value) => value, None => "bad" })
}
''', b"10\nlost\nawaited\n")

    def test_static_rejections(self):
        for body, code, fragment in (
            ("fn f(v: Option<Int?>): Int { match v { None => 0, Some(Some(x)) => x } }", "LYT0012", "Some(None)"),
            ("fn f(v: Int?): Result<Int,String> { Ok(v?) }", "LYT0013", "Option"),
            ("fn f(v: Result<Int,Bool>): Result<Int,String> { Ok(v?) }", "LYT0013", "Bool"),
            ("fn f(v: Int?): Int { match v { Err(_) => 1, _ => 0 } }", "LYT0011", "Err"),
            ("fn f(v: Int?): Int { match v { Some(value) => value, None => value } }", "LYN0001", "value"),
            ("struct Loop(val nested: Option<Loop>)", "LYT0007", "Loop"),
            ("async fn read(): Result<Int,String> { Ok(2) }\nasync fn bad(): Result<Int,String> { Ok(await (read()?)) }", "LYT0013", "Task"),
            ("fn f(v: Result<Int,String>): Bool { v == v }", "LYT0006", "Eq"),
        ):
            with self.subTest(body=body):
                self.rejects(body + "\nfn main(): Unit {}", code, fragment)


if __name__ == "__main__":
    unittest.main()
