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

    def test_near_enum_variant_warns_only_for_an_unescaped_binding(self):
        self.main.write_text('''
enum Color { Red, Blue }
fn typo(color: Color): Color { match color { Bule => Bule } }
fn escaped(color: Color): Color { match color { `Bule` => Bule } }
fn ordinary(color: Color): Color { match color { blue => blue } }
fn main(): Unit {}
''', encoding='utf-8')
        result = subprocess.run([COMPILER, 'check', self.package, '--diagnostics=json'], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        warning, = json.loads(result.stderr)
        self.assertEqual(warning['code'], 'LYW0003')
        self.assertIn('Blue', warning['message'])

    def test_value_equality_is_structural_for_records_and_sums(self):
        self.native('''
struct User(val id: Int, val name: String)
enum Tree { Empty, Branch(Int, Tree) }
fn first(): Result<User,String> { Ok(User(7, "Ada")) }
fn second(): Result<User,String> { Ok(User(7, "Ada")) }
fn failure(): Result<User,String> { Err("bad") }
fn sameTree(): Bool { Tree.Branch(1, Tree.Branch(2, Tree.Empty)) == Tree.Branch(1, Tree.Branch(2, Tree.Empty)) }
const saved: Bool = User(7, "Ada") == User(7, "Ada") && Some(User(7, "Ada")) == Some(User(7, "Ada"))
fn main(): Unit {
    print(saved)
    print(first() == second())
    print(sameTree())
    val nested: Option<String?> = Some(None)
    print(nested == Some(None))
    print(failure() == failure())
    print(User(7, "Ada") != User(7, "Grace"))
}
''', b"true\ntrue\ntrue\ntrue\ntrue\ntrue\n")

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

    def test_guards_run_only_after_a_match_and_keep_bindings_across_await(self):
        self.native(r'''
class Counter(var subjects: Int, var guards: Int)
class Label(val text: String)
struct Ticket(val id: Int, val label: Label)
fn issue(counter: Counter, id: Int): Result<Ticket?,String> {
    counter.subjects += 1
    if id < 0 { Err("bad\0id") }
    else if id == 0 { Ok(None) }
    else { Ok(Some(Ticket(id, Label("ticket")))) }
}
async fn allow(counter: Counter, id: Int): Bool {
    await blocking { gcCollect() }
    counter.guards += 1
    id == 1
}
async fn decide(counter: Counter, id: Int): String {
    match issue(counter, id) {
        Ok(Some(Ticket(id: number, label: label))) if await allow(counter, number) => label.text,
        Ok(Some(Ticket(id: 2))) => "fallback",
        Ok(Some(_)) => "unexpected",
        Ok(null) => "empty",
        Err("bad\0id") => "bad input",
        Err(_) => "unexpected error",
    }
}
async fn main(): Unit {
    val count = Counter(0, 0)
    print(await decide(count, 1))
    print(await decide(count, 2))
    print(await decide(count, 0))
    print(await decide(count, -1))
    print(match -27 { -27 => "negative", _ => "wrong" })
    print(count.subjects)
    print(count.guards)
}
''', b"ticket\nfallback\nempty\nbad input\nnegative\n4\n2\n")

    def test_exhaustiveness_accounts_for_overlapping_fields(self):
        self.native('''
struct Flags(val first: Bool, val second: Bool)
fn mark(flags: Flags): Int {
    match flags {
        Flags(first: true) => 1,
        Flags(second: true) => 2,
        Flags(first: false, second: false) => 3,
    }
}
fn main(): Unit {
    print(mark(Flags(true, true)))
    print(mark(Flags(false, true)))
    print(mark(Flags(false, false)))
    print(match true { true => 4, false => 0 })
}
''', b"1\n2\n3\n4\n")

    def test_enum_variants_with_named_and_positional_payloads(self):
        self.native('''
enum FetchResult { Pending, Ok(status: Int, body: String), Err(String) }
fn status(value: Int): Int { print(value); value }
fn body(value: String): String { print(value); value }
fn display(result: FetchResult): String {
    match result {
        FetchResult.Pending => "pending",
        Ok(body: text, status: 200) => text,
        Ok(code, _) => { print(code); "not found" },
        Err(message) => message,
    }
}
fn main(): Unit {
    print(display(FetchResult.Pending))
    print(display(FetchResult.Ok(body: body("arrived"), status: status(200))))
    print(display(FetchResult.Ok(404, "missing")))
    print(display(FetchResult.Err("failed")))
}
''', b"pending\narrived\n200\narrived\n404\nnot found\nfailed\n")

    def test_recursive_enum_payloads_survive_await_and_gc(self):
        self.native('''
class Label(val text: String)
struct Entry(val label: Label?)
enum Chain { End, Link(Entry, Chain) }
fn chain(): Chain { Chain.Link(Entry(Label("rooted")), Chain.Link(Entry(null), Chain.End)) }
fn exhaust(value: Chain): Int {
    match value { End => 0, Link(_, End) => 1, Link(_, Link(_, _)) => 2 }
}
async fn main(): Unit {
    val value = chain()
    await blocking { gcCollect() }
    print(exhaust(value))
    print(match value {
        Link(Entry(label: Some(label)), Link(Entry(label: None), End)) => label.text,
        _ => "lost",
    })
}
''', b"2\nrooted\n")

    def test_or_patterns_and_integer_ranges(self):
        self.native('''
enum Message { Text(String), Failure(String), Done }
enum Wrapped { Value(Int) }
fn text(message: Message): String {
    match message {
        Text(value) | Failure(value) => value,
        Done => "done",
    }
}
fn classify(value: Int): Int {
    match value {
        -10..<0 => 1,
        0..2 => 2,
        _ => 3,
    }
}
fn byteClass(value: Int8): Int {
    match value {
        -128..-1 => 4,
        0..127 => 5,
    }
}
fn main(): Unit {
    print(text(Message.Text("body")))
    print(text(Message.Failure("error")))
    print(text(Message.Done))
    print(match Wrapped.Value(2) { Wrapped.Value(1 | 2) => 6, _ => 0 })
    print(classify(-10)); print(classify(-1)); print(classify(0)); print(classify(2)); print(classify(3))
    print(byteClass(-128)); print(byteClass(-1)); print(byteClass(0)); print(byteClass(127))
    print(match true { true | false => 7 })
}
''', b"body\nerror\ndone\n6\n1\n1\n2\n2\n3\n4\n4\n5\n5\n7\n")

    def test_class_patterns(self):
        self.native('''
class Gate(val label: String, val open: Bool)
fn state(gate: Gate): Int {
    match gate {
        Gate(open: false) => 0,
        Gate(open: true) => 1,
        _ => 2,
    }
}
fn main(): Unit {
    print(state(Gate("closed", false)))
    print(state(Gate("open", true)))
}
''', b"0\n1\n")

    def test_if_let(self):
        self.native('''
fn describe(value: Option<String>): String {
    if let Some(text) = value { text } else { "empty" }
}
fn main(): Unit {
    print(describe(Some("ready")))
    print(describe(null))
    var count = 0
    val absent: Option<String> = null
    if let Some(_) = absent { count = 1 }
    print(count)
}
''', b"ready\nempty\n0\n")

    def test_list_patterns(self):
        self.native('''
fn describe(items: List<Int>): Int {
    match items {
        [] => 0,
        [first, ..rest] => {
            rest[0] = 99
            first + rest[0]
        },
    }
}
fn exactLength(items: List<Int>): String {
    match items {
        [] => "empty",
        [one] => "one",
        [one, two] => "two",
        _ => "many",
    }
}
fn main(): Unit {
    val values = [5, 6, 7]
    print(describe(values))
    print(values[1])
    print(exactLength([]))
    print(exactLength([1]))
    print(exactLength([1, 2]))
    print(exactLength([1, 2, 3]))
}
''', b"104\n6\nempty\none\ntwo\nmany\n")

    def test_constant_patterns(self):
        self.native('''
const Timeout: Int = 3
const Enabled: Bool = true
fn classify(value: Int): String {
    match value {
        Timeout => "timeout",
        0..2 => "small",
        _ => "other",
    }
}
fn enabled(value: Bool): String {
    match value {
        Enabled => "enabled",
        false => "disabled",
    }
}
fn bind(value: Int): Int {
    match value { `Timeout` => Timeout }
}
fn main(): Unit {
    print(classify(3))
    print(classify(2))
    print(classify(9))
    print(enabled(true))
    print(enabled(false))
    print(bind(42))
}
''', b"timeout\nsmall\nother\nenabled\ndisabled\n42\n")

    def test_irrefutable_val_destructuring(self):
        self.native('''
struct Point<T>(val position: T)
class Token(val label: String)
enum Either { Left(Int), Right(Int) }
const On: Bool = true
fn source(): Point<Int> { print("once"); Point(42) }
fn unwrap(value: Either): Int {
    val Left(number) | Right(number) = value
    number
}
const fn fromConstant(): Int {
    val Point(position: result) = Point(4)
    val On | false = true
    result
}
const derived: Int = fromConstant()
fn main(): Unit {
    val Point(position: number): Point<Int> = source()
    val Token(label: name) = Token("token")
    val original = [1, 2]
    val [..`_`] = original
    `_`[0] = 50
    val _ = source()
    print(name); print(number); print(original[0]); print(`_`[0]); print(unwrap(Either.Right(17))); print(derived)
}
''', b"once\nonce\ntoken\n42\n1\n50\n17\n4\n")

    def test_static_rejections(self):
        for body, code, fragment in (
            ("fn f(v: Option<Int?>): Int { match v { None => 0, Some(Some(x)) => x } }", "LYT0012", "Some(None)"),
            ("fn f(v: Int?): Result<Int,String> { Ok(v?) }", "LYT0013", "Option"),
            ("fn f(v: Result<Int,Bool>): Result<Int,String> { Ok(v?) }", "LYT0013", "Bool"),
            ("fn f(v: Int?): Int { match v { Err(_) => 1, _ => 0 } }", "LYT0011", "Err"),
            ("fn f(v: Int?): Int { match v { Some(value) => value, None => value } }", "LYN0001", "value"),
            ("struct Loop(val nested: Option<Loop>)", "LYT0007", "Loop"),
            ("async fn read(): Result<Int,String> { Ok(2) }\nasync fn bad(): Result<Int,String> { Ok(await (read()?)) }", "LYT0013", "Task"),
            ("fn f(v: Bool): Int { match v { true if true => 1, false => 0 } }", "LYT0012", "true"),
            ("fn f(v: Int): Int { match v { 1 => 1 } }", "LYT0012", "-9223372036854775808"),
            ("fn f(v: Bool): Int { match v { value if 1 => 1, _ => 0 } }", "LYT0001", "Bool"),
            ("fn f(v: Bool): Int { match v { 1 => 1, _ => 0 } }", "LYT0011", "Bool"),
            ("fn f(v: Bool): Int { match v { true | 1 => 1, _ => 0 } }", "LYT0011", "整数字面量"),
            ("fn f(v: Int8): Int { match v { -128..0 => 1 } }", "LYT0012", "1"),
            ("fn f(v: Bool): Int { match v { 1..2 => 1, _ => 0 } }", "LYT0011", "整数区间"),
            ("enum Choice { Left(Int), Right(String) }\nfn f(v: Choice): Int { match v { Left(item) | Right(item) => 1 } }", "LYT0011", "相同名称和类型"),
            ("enum Choice { Left(Int), Right(Int) }\nfn f(v: Choice): Int { match v { Left(item) | Right(other) => 1 } }", "LYT0011", "相同名称和类型"),
            ("fn f(v: Int8): Int { match v { 1..<1 => 0 } }", "LYT0012", "-128"),
            ("struct Flags(val first: Bool, val second: Bool)\nfn f(v: Flags): Int { match v { Flags(first: true) => 1, Flags(second: true) => 2 } }", "LYT0012", "Flags(first: false, second: false)"),
            ("struct Flags(val first: Bool, val second: Bool)\nfn f(v: Flags): Int { match v { Flags(first: name, second: name) => 1 } }", "LYN0002", "name"),
            ("struct Flags(val first: Bool)\nfn f(v: Flags): Int { match v { Flags(first: true, first: false) => 1 } }", "LYN0002", "first"),
            ("class Gate(val open: Bool)\nfn f(v: Gate): Int { match v { Gate(open: true) => 1 } }", "LYT0012", "_"),
            ("fn f(values: List<Int>): Int { match values { [first] => first } }", "LYT0012", "[]"),
            ("fn f(value: Int): Int { match value { [first, ..rest] => first } }", "LYT0011", "列表模式"),
            ("fn f(values: List<Int>): Int { match values { [..rest, first] => 1, _ => 0 } }", "LYP0001", "末尾"),
            ("const Open: Bool = true\nfn f(value: Bool): Int { match value { Open => 1 } }", "LYT0012", "false"),
            ("fn f(value: Option<Int>): Int { val Some(number) = value; number }", "LYT0012", "None"),
            ("fn f(values: List<Int>): Int { val [first, ..rest] = values; first }", "LYT0012", "[]"),
            ("class State(val open: Bool)\nfn f(value: State): Unit { val State(open: true) = value }", "LYT0012", "val 解构"),
            ("enum Decision { Left, Right(Bool) }\nfn f(v: Decision): Int { match v { Left => 0, Right(true) => 1 } }", "LYT0012", "Right(false)"),
            ("enum Decision { Left, Right(Bool) }\nfn f(v: Decision): Int { match v { Right => 1, _ => 0 } }", "LYT0011", "Right"),
            ("enum Decision { Left }\nfn f(): Decision { Decision.Lost }", "LYN0001", "Lost"),
            ("enum Decision { Right(Bool) }\nfn f(): Decision { Decision.Right }", "LYT0002", "Right"),
            ("enum Decision { Left, Left }", "LYN0002", "Left"),
            ("enum Decision { Right(value: Bool, value: Int) }", "LYN0002", "value"),
            ("enum Decision { Left }\nenum Other { Left }\nfn f(v: Decision): Int { match v { Other.Left => 1, _ => 0 } }", "LYT0011", "Decision"),
        ):
            with self.subTest(body=body):
                self.rejects(body + "\nfn main(): Unit {}", code, fragment)


if __name__ == "__main__":
    unittest.main()
