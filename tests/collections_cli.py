import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class Collections(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.package = Path(temporary.name)
        self.src = self.package / "src"
        self.src.mkdir()
        self.main = self.src / "main.ly"

    def native(self, source, expected, error=None):
        self.main.write_text(source, encoding="utf-8")
        for flags in ([], ["--release"]):
            with self.subTest(flags=flags):
                executable = self.package / "program"
                build = subprocess.run([COMPILER, "build", self.package, *flags, "-o", executable], capture_output=True, timeout=30)
                self.assertEqual(build.returncode, 0, build.stderr.decode())
                run = subprocess.run([executable], capture_output=True, timeout=10)
                self.assertEqual(run.stdout, expected)
                if error is None:
                    self.assertEqual((run.returncode, run.stderr), (0, b""))
                else:
                    self.assertNotEqual(run.returncode, 0)
                    self.assertIn(error, run.stderr)

    def test_defaults_bind_after_explicit_arguments_in_declaration_order(self):
        self.native('''
fn mark(label: String, value: Int): Int { print(label); value }
fn join(left: Int = mark("L", 1), right: Int = mark("R", left + 1)): Int { left * 10 + right }
fn main(): Unit {
    print(join(right: mark("A", 4), left: mark("B", 2)))
    print(join(right: mark("X", 9)))
    print(join())
}
''', b"A\nB\n24\nX\nL\n19\nL\nR\n12\n")

    def test_standard_higher_order_collection_pipeline(self):
        (self.package / 'lyra.json5').write_text('{name:"test/collections",version:"0.1.0"}')
        self.native('''import std.collections.map
import std.collections.filter
import std.collections.fold
fn main(): Unit {
    var visits = 0
    val input = [1, 2, 3, 4]
    val selected = filter(input) { visits += 1; it > 2 }
    val doubled = map(selected) { it * 2 }
    input[2] = 100
    print(fold(doubled, 0, (total: Int, value: Int) => { total + value }))
    val empty: List<String> = []
    print(fold(empty, "zero", (total: String, value: String) => { total + value }))
    print(visits)
}''', b'14\nzero\n4\n')

    def test_async_defaults_run_before_the_cold_task_starts(self):
        self.native('''
fn mark(value: Int): Int { print(value); value }
async fn work(value: Int = mark(3)): Int { print(4); value }
async fn main(): Unit { val cold = work(); print(5); print(await cold); print(await cold) }
''', b"3\n5\n4\n3\n3\n")

    def test_generic_and_method_defaults_use_declaration_scope(self):
        self.native('''
struct Start(val base: Int) {
    fn count(self, first: Int = self.base, values: Int...): Int { first + values.size }
}
fn collect<T>(item: T, values: List<T> = [item]): List<T> { values }
fn main(): Unit {
    print(Start(11).count(...[3, 4]))
    print(collect("Ada")[0])
}
''', b"13\nAda\n")

    def test_spread_copies_slots_but_not_referenced_objects(self):
        self.native('''
class Contact(var name: String)
fn change(replacement: Contact, values: Contact...): Contact {
    values[0].name = "Ada"
    values[0] = replacement
    values[0]
}
fn collect<T>(values: T...): List<T> { values }
fn identity<T>(value: List<T>): List<T> { value }
fn main(): Unit {
    val original = Contact("Lin")
    val contacts = [original]
    print(change(Contact("new"), ...contacts).name)
    print(contacts[0].name)
    val other = identity(contacts)
    other[0] = Contact("shared")
    print(contacts[0].name)
    val first: List<Int> = collect()
    val second = collect<Int>()
    first.add(7)
    print(second.size)
    val nested = collect(first)
    print(nested[0][0])
    val flat = collect(...first)
    print(flat[0])
}
''', b"new\nAda\nshared\n0\n7\n7\n")

    def test_spreads_snapshot_in_order_and_only_once(self):
        self.native('''
fn choose(items: List<Int>, marker: Int): List<Int> { print(marker); items }
fn change(items: List<Int>): Int { items[0] = 9; 3 }
fn pack(values: Int...): List<Int> { values }
fn main(): Unit {
    val items = [1, 2]
    val packed = pack(...choose(items, 4), change(items), ...choose(items, 5))
    var index = 0
    while index < packed.size { print(packed[index]); index += 1 }
}
''', b"4\n5\n1\n2\n3\n9\n2\n")

    def test_index_assignment_survives_growth_and_holds_gc_references(self):
        self.native('''
struct Entry(var name: String, var version: Int)
fn first(): Int { print(1); 0 }
fn grow(items: List<Entry>): String {
    var index = 0
    while index < 32 { items.add(Entry("other", 0)); index += 1 }
    items[0].version = 7
    gcCollect()
    "after"
}
fn main(): Unit {
    val items = [Entry("before", 0)]
    items[first()].name = grow(items)
    gcCollect()
    print(items[0].name)
    print(items[0].version)
    print(items.size)
}
''', b"1\nafter\n7\n33\n")

    def test_bounds_error_is_located_before_rhs_runs(self):
        self.native('''fn update(): Int { print(9); 4 }
fn main(): Unit {
    val values = [1]
    values[-1] = update()
}
''', b"", b"main.ly:4:")

    def test_static_rejections(self):
        cases = (
            ("fn bad(first: Int = next, next: Int = 1): Int { first }", "", "LYN0001", "next"),
            ("fn bad(values: Int..., tail: Int): Unit {}", "", "LYP0001", "变长"),
            ("fn many(items: Int...): Unit {}", "many(items: [1])", "LYT0002", "变长"),
            ("fn many(items: Int...): Unit {}", "many([1])", "LYT0001", "List"),
            ("fn many(items: Int...): Unit {}", "many(...1)", "LYT0001", "List"),
            ("fn only(items: List<Int>): Unit {}", "only(...[1])", "LYT0002", "变长"),
            ("fn only(text: List<String>): Unit {}", "val ints = [1]; only(ints)", "LYT0001", "List"),
            ("", "val items = []", "LYT0014", "List"),
        )
        for declaration, body, code, fragment in cases:
            with self.subTest(declaration=declaration, body=body):
                self.main.write_text(declaration + "\nfn main(): Unit { " + body + " }", encoding="utf-8")
                result = subprocess.run([COMPILER, "check", self.package, "--diagnostics=json"], capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                diagnostic, = json.loads(result.stderr)
                self.assertEqual(diagnostic["code"], code, diagnostic)
                self.assertIn(fragment, diagnostic["message"])


if __name__ == "__main__":
    unittest.main()
