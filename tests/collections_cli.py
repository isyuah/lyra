import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from platform_cli import executable


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
                app = executable(self.package / ("release" if flags else "debug"))
                build = subprocess.run([COMPILER, "build", self.package, *flags, "-o", app], capture_output=True, timeout=30)
                self.assertEqual(build.returncode, 0, build.stderr.decode())
                run = subprocess.run([app], capture_output=True, timeout=10)
                self.assertEqual(run.stdout, expected)
                if error is None:
                    self.assertEqual((run.returncode, run.stderr), (0, b""))
                else:
                    self.assertNotEqual(run.returncode, 0)
                    for fragment in (error,) if isinstance(error, bytes) else error:
                        self.assertIn(fragment, run.stderr)

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
        self.native('''import std.collections.{map}
import std.collections.{filter}
import std.collections.{fold}
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

    def test_hash_collections_grow_remove_and_preserve_optional_values(self):
        (self.package / 'lyra.json5').write_text('{name:"test/hash-collections",version:"0.1.0"}')
        self.native(r'''
import std.collections.{hashMap}
import std.collections.{intSet}
import std.collections.{stringMap}

fn main(): Unit {
    val map = hashMap<Int,String>((key) => { -1 }, (left, right) => { left == right })
    for key in 0..<20 { map.put(key, "item ${key}"); }
    val alias = map
    print(map.size())
    print(alias.remove(12) ?? "missing")
    print(map.contains(12))
    print(alias.put(7, "updated") ?? "missing")
    val snapshot = map.entries()
    print(snapshot.size)
    print(map.get(7) ?? "missing")
    map.remove(7)
    print(snapshot.size)

    val optional = stringMap<String?>()
    optional.put("key\0é", None)
    gcCollect()
    print(match optional.get("key\0é") { Some(None) => "present null", Some(Some(_)) => "wrong", None => "missing" })
    print(optional.get("other") == None)

    val set = intSet()
    print(set.add(-9223372036854775808))
    print(set.add(-9223372036854775808))
    print(set.remove(-9223372036854775808))
    print(set.size())
}
''', b"20\nitem 12\nfalse\nitem 7\n19\nupdated\n19\npresent null\ntrue\ntrue\nfalse\ntrue\n0\n")

    def test_structural_hash_keys_and_custom_equality(self):
        (self.package / 'lyra.json5').write_text('{name:"test/hash-derived",version:"0.1.0"}')
        self.native('''import std.collections.{hashSet}
struct User(val id: Int, val name: String)
struct UserKey(val id: Int, val name: String) {
    fn equals(other: UserKey): Bool { id == other.id }
    fn hashCode(): Int { id }
}
struct WrappedKey(val key: UserKey, val shard: Int)
enum Tree { Empty, Node(Int, Tree) }
fn equivalent<T: Eq>(left: T, right: T): Bool { left.equals(right) }
fn hashValue<T: Hash>(value: T): Int { value.hashCode() }
const knownEqual: Bool = equivalent(User(5, "Ada"), User(5, "Ada"))
const knownHash: Int = hashValue(User(5, "Ada"))
fn main(): Unit {
    print(knownEqual)
    print(knownHash == User(5, "Ada").hashCode())
    val users = hashSet<User>()
    print(users.add(User(1, "Ada")))
    print(users.add(User(1, "Ada")))
    print(users.contains(User(1, "Ada")))
    print(User(1, "Ada").hashCode() == User(1, "Ada").hashCode())
    val custom = hashSet<UserKey>()
    print(custom.add(UserKey(3, "first")))
    print(custom.add(UserKey(3, "second")))
    print(custom.size())

    val wrapped = hashSet<WrappedKey>()
    print(wrapped.add(WrappedKey(UserKey(4, "first"), 1)))
    print(wrapped.add(WrappedKey(UserKey(4, "second"), 1)))

    val trees = hashSet<Tree>()
    print(trees.add(Tree.Node(1, Tree.Node(2, Tree.Empty))))
    print(trees.add(Tree.Node(1, Tree.Node(2, Tree.Empty))))
}
''', b'true\ntrue\ntrue\nfalse\ntrue\ntrue\ntrue\nfalse\n1\ntrue\nfalse\ntrue\nfalse\n')

    def test_hash_capability_rejects_non_hashable_struct_fields(self):
        cases = [
            ('struct Key(val value: Any)\nfn main(): Unit { val items = hashSet<Key>() }', 'value'),
            ('import std.io.{File}\nstruct Key(val file: File)\nfn main(): Unit { val items = hashSet<Key>() }', 'file'),
            ('struct Key(val callback: CCallback)\nfn main(): Unit { val items = hashSet<Key>() }', 'callback'),
            ('class Handle {}\nstruct Key(val value: Handle)\nfn main(): Unit { val items = hashSet<Key>() }', 'value'),
            ('struct Key(val callback: () -> Int)\nfn main(): Unit { val items = hashSet<Key>() }', 'callback'),
            ('trait Render { fn render(): String }\nstruct Key(val value: dyn Render)\nfn main(): Unit { val items = hashSet<Key>() }', 'value'),
            ('class Handle {}\nfn main(): Unit { val items = hashSet<Handle>() }', 'Hash'),
            ('struct Key(val id: Int) { fn equals(other: Key): Bool { id == other.id } }\nfn main(): Unit { val items = hashSet<Key>() }', 'hashCode'),
            ('struct Key(val id: Int) { fn hashCode(): Int { id } }\nfn main(): Unit { val items = hashSet<Key>() }', 'equals'),
            ('struct Key(val id: Int) { fn hashCode(): Int { id } }\nfn main(): Unit { val items = hashSet<Result<Key?,Int>>() }', 'equals'),
            ('struct Key(val id: Int) { fn hashCode(): Int { id } }\nenum Outer { Found(Key), Missing }\nfn main(): Unit { val items = hashSet<Outer>() }', 'equals'),
        ]
        (self.package / 'lyra.json5').write_text('{name:"test/hash-rejections",version:"0.1.0"}')
        for body, fragment in cases:
            with self.subTest(body=body):
                self.main.write_text('import std.collections.{hashSet}\n' + body, encoding='utf-8')
                check = subprocess.run([COMPILER, 'check', self.package, '--diagnostics=json'], capture_output=True, text=True, timeout=10)
                self.assertNotEqual(check.returncode, 0, check.stdout)
                error, = json.loads(check.stderr)
                self.assertEqual(error['code'], 'LYT0019', error)
                self.assertIn(fragment, error['message'])

    def test_explicit_hash_is_independent_of_eq(self):
        self.native('''struct Key(val id: Int, val callback: () -> Int) {
    fn hashCode(): Int { id }
}
fn hash<T: Hash>(value: T): Int { value.hashCode() }
fn main(): Unit {
    val key = Key(19, () => { 0 })
    print(hash(key))
    print(hash(Some(key)) == Some(key).hashCode())
}
''', b'19\ntrue\n')

    def test_class_equality_uses_identity(self):
        self.native('''class Handle(val id: Int) {}
fn main(): Unit {
    val first = Handle(1)
    val alias = first
    val second = Handle(1)
    print(first == alias)
    print(first == second)
    print(first != second)
}
''', b'true\nfalse\ntrue\n')

    def test_hash_collection_loops_traverse_shallow_snapshots(self):
        (self.package / 'lyra.json5').write_text('{name:"test/hash-iteration",version:"0.1.0"}')
        self.native('''
import std.collections.{hashSet}
import std.collections.{intMap}

fn main(): Unit {
    val scores = intMap<Int>()
    val set = hashSet<Int?>((value) => { value ?? -1 }, (left, right) => { (left ?? -1) == (right ?? -1) })
    var emptyVisits = 0
    for entry in scores { emptyVisits += 1 }
    for value in set { emptyVisits += 1 }
    print(emptyVisits)

    scores.put(2, 20)
    scores.put(5, 50)
    var mapVisits = 0
    var keySum = 0
    var scoreSum = 0
    for entry in scores {
        mapVisits += 1
        if mapVisits == 1 { scores.remove(2); scores.put(5, 500); scores.put(9, 90); }
        keySum += entry.key
        scoreSum += entry.value
    }
    print(mapVisits); print(keySum); print(scoreSum)

    set.add(None)
    set.add(7)
    var setVisits = 0
    var nullVisits = 0
    var sum = 0
    for value in set {
        setVisits += 1
        if setVisits == 1 { set.remove(None); set.remove(7); set.add(10); }
        if value == None { nullVisits += 1 }
        sum += value ?? 0
    }
    print(setVisits); print(nullVisits); print(sum)
    for value in set { print(value ?? -1) }
}
''', b"0\n2\n7\n70\n2\n1\n7\n10\n")

    def test_generic_and_method_defaults_use_declaration_scope(self):
        self.native('''
struct Start(val base: Int) {
    fn count(first: Int = base, values: Int...): Int { first + values.size }
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

    def test_range_values_and_list_slices(self):
        self.native('''
class Item(var value: Int)
fn mark(value: Int): Int { print(value); value }
fn take<T>(items: List<T>, portion: Range<Int>): List<T> { items.slice(portion) }
fn total(portion: Range<Int>): Int {
    var result = 0
    for number in portion { result += number }
    result
}
const saved: Range<Int> = 0 + 1..<2 * 2
const first: Int = [8, 9, 10, 11].slice(saved)[0]
const same: Bool = saved == (1..<4)
fn main(): Unit {
    val range = mark(1)..mark(2)
    print(range.start); print(range.end); print(range.inclusive)
    print(total(range)); print(total(saved)); print(total(3..1)); print(total(4..<4))
    print(first); print(same)
    val limit = 9223372036854775807
    for number in limit..limit { print(number); continue }
    val input = [Item(10), Item(20), Item(30)]
    val copy = take(input, range)
    copy[0].value = 21
    copy[1] = Item(99)
    input.add(Item(40))
    gcCollect()
    print(input[1].value); print(input[2].value); print(copy[1].value)
    print(copy.size); print(input.slice(4..<4).size)
}
''', b"1\n2\n1\n2\ntrue\n3\n6\n0\n0\n9\ntrue\n9223372036854775807\n21\n30\n99\n2\n0\n")

    def test_list_slice_checks_bounds_at_call_site(self):
        self.native('''fn main(): Unit {
    val items = [1, 2]
    items.slice(0..9223372036854775807);
}
''', b"", (b"BoundsError: list slice 0..9223372036854775807 out of bounds (size 2)", b"main.ly:3:11"))

    def test_remove_at_returns_element_and_mutates_aliases(self):
        self.native('''
const fn shift(): Int {
    val input = [10, 20, 30]
    input.removeAt(1) + input[1] + input.size
}
const folded: Int = shift()
fn main(): Unit {
    val input = ["first", "second", "third"]
    val alias = input
    print(alias.removeAt(index: 1))
    gcCollect()
    print(input.size); print(input[1]); print(folded)
    input.add("last")
    for value in input { print(value) }
}
''', b"second\n2\nthird\n52\nfirst\nthird\nlast\n")

    def test_remove_at_preserves_iteration_length_and_rechecks_writes(self):
        self.native('''fn main(): Unit {
    val values = [1, 2]
    for value in values { print(value); values.removeAt(1); }
}
''', b"1\n", b"list index 1 out of bounds (size 1)")
        self.native('''fn main(): Unit {
    val values = [1, 2]
    values[1] = values.removeAt(0)
}
''', b"", b"list index 1 out of bounds (size 1)")
        for body in ('values[1] = values.removeAt(0)', 'for item in values { values.removeAt(1); }'):
            with self.subTest(body=body):
                self.main.write_text(f'const fn bad(): Int {{ val values = [1, 2]; {body}; 0 }}\nconst value: Int = bad()\nfn main(): Unit {{}}', encoding='utf-8')
                result = subprocess.run([COMPILER, 'check', self.package, '--diagnostics=json'], capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(json.loads(result.stderr)[0]['code'], 'LYCT0003')

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
            ("", "val range: Range<String> = 0..1", "LYT0003", "Range<Int>"),
            ("", "val range = 0..1..2", "LYP0001", "区间"),
            ("", "val range = 0..<true", "LYT0001", "Int"),
            ("", "[1].slice(0)", "LYT0001", "Range<Int>"),
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
