import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from platform_cli import assert_linkage, executable


COMPILER = Path(sys.argv.pop(1)).resolve()
GC_FIXTURE = Path(__file__).parent / "fixtures" / "objects" / "src" / "main.ly"


class Objects(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.package = Path(temporary.name)
        self.src = self.package / "src"
        self.src.mkdir()
        self.main = self.src / "main.ly"

    def run_source(self, source, expected):
        self.main.write_text(source, encoding="utf-8")
        for profile, options in (("debug", []), ("release", ["--release"])):
            with self.subTest(profile=profile):
                app = executable(self.package / profile)
                built = subprocess.run([COMPILER, "build", self.package, *options, "-o", app],
                                       capture_output=True, timeout=30)
                self.assertEqual(built.returncode, 0, built.stderr.decode())
                result = subprocess.run([app], capture_output=True, timeout=20)
                self.assertEqual((result.returncode, result.stdout, result.stderr), (0, expected, b""))
                assert_linkage(self, app)

    def test_copy_fields_method_and_identity(self):
        (self.src / "models.ly").write_text("""
class User(var name: String) {
    fn label(): String { name }
    fn rename(name: String): Unit { this.name = name }
}
struct Profile { var title: String; val owner: User }
struct Wrapper(val profile: Profile)
""", encoding="utf-8")
        self.run_source("""
fn main(): Unit {
    val original = Profile(owner: User("Lin"), title: "原稿")
    var copy = original
    copy.title = "修订"
    val wrapper = Wrapper(copy)
    wrapper.profile.owner.rename("Ada")
    print(original.title)
    print(copy.title)
    print(original.owner.label())
    if original.owner == copy.owner { print(1) }
    if original.owner != User("Ada") { print(2) }
}
""", "原稿\n修订\nAda\n1\n2\n".encode())

    def test_self_computed_properties_and_enum_method_values(self):
        self.run_source('''
trait Duplicate { fn duplicate(): Self }
fn duplicate<T: Duplicate>(item: T): T { item.duplicate() }
struct Box<T>(val data: T) {
    val contents: T get() { data }
    fn duplicate(): Self { this }
    fn value(): T { contents }
}
enum Load<T> { Wait, Ready(T);
    fn duplicate(): Self { this }
    fn value(fallback: T): T { match this { Wait => fallback, Ready(item) => item } }
    fn label(): String { "ready" }
    fn label(prefix: String): String { prefix + label() }
    fn equals(other: Self): Bool { label() == other.label() }
    fn hashCode(): Int { 7 }
}
fn main(): Unit {
    val box = duplicate(Box(42))
    print(box.contents)
    print(box == Box(42))
    val load = duplicate(Load.Ready(box.value()))
    val read: (Int) -> Int = load.value
    val format: (String) -> String = load.label
    print(read(0))
    print(format("is "))
    print(Load<Int>.Wait == Load.Ready(0))
}
''', b'42\ntrue\n42\nis ready\ntrue\n')

    def test_self_trait_is_static_and_getter_is_read_only(self):
        for program, code in [
            ('trait Duplicate { fn duplicate(): Self }\nfn use(value: dyn Duplicate): Unit {}', 'LYG0007'),
            ('class Box { val item: Int get() { 7 } }\nfn use(box: Box): Unit { box.item = 10 }', 'LYT0004'),
            ('class Box { val item: Int; val item: Int get() { 7 } }', 'LYN0002'),
        ]:
            with self.subTest(program=program):
                self.main.write_text(program + '\nfn main(): Unit {}', encoding='utf-8')
                result = subprocess.run([COMPILER, 'check', self.package, '--diagnostics=json'], capture_output=True, timeout=25)
                self.assertNotEqual(result.returncode, 0)
                diagnostic, = json.loads(result.stderr)
                self.assertEqual(diagnostic['code'], code, diagnostic)

    def test_heap_roots_strings_and_inline_data(self):
        self.run_source(GC_FIXTURE.read_text(encoding="utf-8"), "持有\nA\0B\n1\n2\n".encode())

    def test_reference_parameters_keep_slots_and_value_arguments_keep_snapshots(self):
        (self.package / 'lyra.json5').write_text('''{
            name:"test/refs", version:"0.1.0",
            dependencies:{models:{name:"test/models", version:"^0.1.0", path:"models"}}
        }''')
        library = self.package / 'models'
        (library / 'src').mkdir(parents=True)
        (library / 'lyra.json5').write_text('{name:"test/models", version:"0.1.0", kind:"lib"}')
        (library / 'src' / 'packet.ly').write_text('''
pub struct Packet(pub var text: String, pub var id: Int, pub val values: List<Int>, pub val backup: List<Int>)
pub class Storage(pub var packet: Packet)
pub fn check(packet: Packet): Int { packet.id + 10 }
pub fn check(in packet: Packet): Int { packet.id }
pub fn forward<T>(in value: T): T { value }
pub fn change(ref packet: Packet, id: Int = packet.id + 1): Int {
    packet.id = id
    gcCollect()
    packet.id
}
pub fn alias(ref first: Packet, in second: Packet): Int {
    first.id += 1
    second.id
}
pub fn capturedValue(packet: Packet, changed: Int): Int { packet.id }
pub class Editor {
    pub init() {}
    pub fn rewrite(ref packet: Packet): Unit { packet.text = "changed" }
}
''', encoding='utf-8')
        self.run_source('''
import models
fn main(): Unit {
    val store = models.Storage(models.Packet("原稿", 1, [41], [42]))
    print(models.capturedValue(store.packet, models.change(packet: ref store.packet)))
    print(models.check(in store.packet))
    print(models.check(store.packet))
    print(models.alias(ref store.packet, in store.packet))
    val previous = models.forward(in store.packet)
    models.Editor().rewrite(ref store.packet)
    print(previous.text)
    print(store.packet.text)
    print(previous.values[0] + store.packet.backup[0])
}
''', '1\n2\n12\n3\n原稿\nchanged\n83\n'.encode())

    def test_reference_parameter_rejections(self):
        declarations = '''
struct Packet(var id: Int)
class Holder(val packet: Packet) {
    val computed: Packet get() { packet }
}
fn inspect(in packet: Packet): Int { packet.id }
fn edit(ref packet: Packet): Unit { packet.id += 1 }
fn widen(in value: Int): Int { value }
'''
        cases = [
            ('val packet = Packet(1); edit(ref packet)', 'LYT0022'),
            ('var packet = Packet(1); edit(packet)', 'LYT0022'),
            ('var packet = Packet(1); edit(in packet)', 'LYT0022'),
            ('val _ = inspect(in Packet(1))', 'LYT0022'),
            ('val holder = Holder(Packet(1)); val _ = inspect(in holder.computed)', 'LYT0022'),
            ('val packets = [Packet(1)]; val _ = inspect(in packets[0])', 'LYT0022'),
            ('val small: Int8 = 7; val _ = widen(in small)', 'LYT0001'),
            ('val call = inspect', 'LYT0022'),
            ('var number = 1; print(ref number)', 'LYT0022'),
            ('val number = 1; val _ = Some(in number)', 'LYT0022'),
        ]
        programs = [('fn main(): Unit { ' + body + ' }', code) for body, code in cases]
        for declaration, code in (
            ('fn captured(in packet: Packet): () -> Packet { () => { packet } }', 'LYT0022'),
            ('fn deferred(ref packet: Packet): Unit { defer { packet.id = 0 } }', 'LYT0022'),
            ('fn readonly(in packet: Packet): Unit { packet.id = 0 }', 'LYT0004'),
            ('async fn later(in packet: Packet): Int { packet.id }', 'LYT0022'),
            ('const fn fromSlot(in packet: Packet): Packet { packet }', 'LYCT0001'),
        ):
            programs.append((declaration + '\nfn main(): Unit {}', code))
        for program, code in programs:
            with self.subTest(program=program):
                self.main.write_text(declarations + '\n' + program, encoding='utf-8')
                result = subprocess.run([COMPILER, 'check', self.package, '--diagnostics=json'], capture_output=True, timeout=25)
                self.assertNotEqual(result.returncode, 0)
                diagnostic, = json.loads(result.stderr)
                self.assertEqual(diagnostic['code'], code, diagnostic)

    def test_order_and_single_receiver_evaluation(self):
        self.run_source("""
class Counter(var value: Int)
struct Point(var left: Int, var right: Int)
fn next(counter: Counter): Int { counter.value += 1; counter.value }
fn once(counter: Counter): Counter { print(9); counter }
fn main(): Unit {
    val counter = Counter(0)
    var point = Point(right: next(counter), left: next(counter))
    once(counter).value += 5
    print(point.left)
    print(point.right)
    print(counter.value)
}
""", b"9\n2\n1\n7\n")

    def test_field_defaults_use_declaration_scope_and_constructor_order(self):
        (self.package / 'lyra.json5').write_text('{name:"test/defaults", version:"0.1.0"}')
        models = self.src / 'models'
        models.mkdir()
        (models / 'packet.ly').write_text('''
fn mark(value: Int): Int { print(value); value }
pub struct Packet<Item>(pub val item: Item, val base: Int = mark(2)) {
    pub fn first(): Item { items[0] }
    pub val items: List<Item> = [item]
    pub val total: Int = mark(base + 1)
}
pub class Counter<Item> {
    pub val seed: Int = mark(4)
    pub init(item: Item, seed: Int) { this.item = item; count += seed }
    pub val item: Item
    pub var count: Int = seed + 1
}
''', encoding='utf-8')
        self.run_source('''
import self.models.{Packet as Envelope}
import self.models.{Counter}
fn mark(value: Int): Int { -1 }
fn supplied(value: Int): Int { print(value); value }
fn main(): Unit {
    val packet = Envelope(total: supplied(9), item: supplied(7))
    print(packet.total); print(packet.first())
    val other: Envelope<String> = Envelope("Ada")
    print(other.first()); print(other.total)
    val counter = Counter("init", 6)
    print(counter.count); print(counter.item)
}
''', b'9\n7\n2\n9\n7\n2\n3\nAda\n3\n4\n11\ninit\n')
        self.main.write_text('import self.models.{Packet}\nfn main(): Unit { Packet(7, base: 99); }')
        result = subprocess.run([COMPILER, 'check', self.package, '--diagnostics=json'], capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        error, = json.loads(result.stderr)
        self.assertEqual(error['code'], 'LYN0003')
        self.assertIn('base', error['message'])

    def test_class_defaults_allocate_per_instance_and_propagate_exceptions(self):
        self.run_source('''
fn initialItems(): List<Int> { print("new"); [] }
class Bucket(val items: List<Int> = initialItems(), val count: Int = items.size)
fn fail(): Int { throw Exception("default failed") }
class Attempt { val value: Int = fail(); init() { print("body") } }
fn main(): Unit {
    val first = Bucket(); first.items.add(8)
    val second = Bucket(count: 2)
    val shared = Bucket(items: first.items)
    print(second.items.size); print(second.count); print(shared.count)
    print(try { Attempt().value.toString() } catch error: Exception { error.message })
}
''', b'new\nnew\n0\n2\n1\ndefault failed\n')

    def test_defaults_are_checked_even_without_construction(self):
        for declaration, code, message in (
            ('struct Bad<T>(val wrong: T = 1)', 'LYT0001', 'T'),
            ('struct Bad(val first: Int = second, val second: Int = 2)', 'LYN0001', 'second'),
            ('class Bad(val first: Int = this.second, val second: Int = 2)', 'LYT0018', 'this'),
            ('class Bad { val value: Int = 1; init() { value = 2 } }', 'LYT0017', '只能初始化一次'),
            ('class Bad { val need: Int; val copy: Int = need; init() { need = 1 } }', 'LYN0001', 'need'),
        ):
            with self.subTest(declaration=declaration):
                self.main.write_text(declaration + '\nfn main(): Unit {}', encoding='utf-8')
                result = subprocess.run([COMPILER, 'check', self.package, '--diagnostics=json'], capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                error, = json.loads(result.stderr)
                self.assertEqual(error['code'], code)
                self.assertIn(message, error['message'])

    def test_extension_property_cannot_read_uninitialized_this(self):
        self.main.write_text('''
extension View for Item { val label: String get() { "item" } }
class Item { val value: Int; init() { print(this.label); value = 1 } }
fn main(): Unit { Item(); }
''', encoding='utf-8')
        result = subprocess.run([COMPILER, 'check', self.package, '--diagnostics=json'],
                                capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        error, = json.loads(result.stderr)
        self.assertEqual(error['code'], 'LYT0017')
        self.assertIn('使用 this', error['message'])

    def test_implicit_receiver_respects_shadowing_defaults_and_capture(self):
        self.run_source('''
fn read(): Int { -1 }
class Counter(var value: Int, val transform: (Int) -> Int) {
    fn read(): Int { value }
    fn update(value: Int, read: () -> Int): Int {
        this.value = value
        {
            val value = read()
            print(value)
        }
        transform(read() + this.read())
    }
    fn bump(by: Int = value): Int { value += by; read() }
    fn callback(): () -> Int { () => { read() + this.value } }
}
struct Pair(val left: Int, val right: Int) {
    static fn doubled(value: Int): Pair { Pair(value, value) }
    fn sum(): Int { left + this.right }
}
fn main(): Unit {
    val counter = Counter(3, (value: Int) => { value + 1 })
    print(counter.update(5, () => { 9 }))
    val callback = counter.callback()
    print(counter.bump())
    gcCollect()
    print(callback())
    print(Pair.doubled(21).sum())
    val Pair = counter
    print(Pair.read())
}
''', b'9\n15\n10\n20\n42\n10\n')

    def test_static_and_instance_calls_require_the_right_receiver(self):
        for source, code, message in (
            ('class Box(val value: Int) { static fn get(): Int { this.value } }', 'LYT0018', 'this'),
            ('class Box(val value: Int) { static fn get(): Int { value } }', 'LYN0001', 'value'),
            ('class Box { static fn get(): Int { 7 } }\nfn use(): Int { Box().get() }', 'LYT0018', '类型名'),
            ('class Box { fn get(): Int { 7 } }\nfn use(): Int { Box.get() }', 'LYT0018', '对象接收者'),
            ('class Box { val value: Int; init(value: Int = this.value) { this.value = value } }', 'LYT0018', 'this'),
        ):
            with self.subTest(source=source):
                self.main.write_text(source + '\nfn main(): Unit {}', encoding='utf-8')
                result = subprocess.run([COMPILER, 'check', self.package, '--diagnostics=json'],
                                        capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                diagnostic, = json.loads(result.stderr)
                self.assertEqual(diagnostic['code'], code)
                self.assertIn(message, diagnostic['message'])

    def test_overloaded_methods_keep_receiver_and_structural_dispatch(self):
        self.run_source('''
trait Renderer { fn render(text: String): String }
struct Label(val value: String)
implicit (label: Label): String { "converted " + label.value }
class Tool<Item>(val item: Item) {
    fn read(value: Item): String { "item" }
    fn read<Other>(value: Other): String { "generic" }
    static fn read(value: String): String { value }
    static fn read(value: Item, extra: Int = 7): Int { extra }
    fn render(text: Label): String { "exact " + text.value }
    fn render(text: String): String { text }
    fn test(value: Item): String { read(value) }
}
fn render<T: Renderer>(tool: T): String { tool.render(Label("through constraint")) }
fn receiver(): Tool<Int> { print("receiver"); Tool(42) }
fn main(): Unit {
    print(receiver().read(8))
    val tool = Tool(0)
    print(tool.test(1))
    print(tool.read(true))
    print(Tool<Int>.read("static"))
    print(Tool<Int>.read(9))
    print(tool.render(Label("direct")))
    print(render(tool))
}
''', b'receiver\nitem\nitem\ngeneric\nstatic\n7\nexact direct\nconverted through constraint\n')

    def test_static_rejections(self):
        types = "struct Point(var x: Int)\nclass Box(val point: Point, var value: Int)\n"
        for body, code in (("val p = Point(1); p.x = 2", "LYT0004"),
                           ("val b = Box(Point(1), 2); b.point.x = 3", "LYT0004"),
                           ("val b = Box(Point(1), 2); b.point = Point(3)", "LYT0004"),
                           ("Point(x: true)", "LYT0001"),
                           ("Point(other: 1)", "LYT0002"),
                           ("val p = Point(1); p.missing", "LYN0001")):
            with self.subTest(body=body):
                self.main.write_text(types + "fn main(): Unit { " + body + "; }", encoding="utf-8")
                result = subprocess.run([COMPILER, "check", self.package, "--diagnostics=json"],
                                        capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                diagnostic, = json.loads(result.stderr)
                self.assertEqual(diagnostic["code"], code)
                self.assertEqual(diagnostic["range"]["start"]["line"], 3)

    def test_structural_equality_rejects_dynamic_fields(self):
        self.main.write_text("struct Key(val value: Any)\nfn main(): Unit { val first = Key(1); val second = Key(1); print(first == second) }", encoding="utf-8")
        result = subprocess.run([COMPILER, "check", self.package, "--diagnostics=json"], capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        diagnostic, = json.loads(result.stderr)
        self.assertEqual(diagnostic["code"], "LYT0006")
        self.assertIn("value", diagnostic["message"])

    def test_inline_layout_cycle_rejected(self):
        self.main.write_text("struct Outer(val inner: Inner)\nstruct Inner(val outer: Outer)\nfn main(): Unit {}")
        result = subprocess.run([COMPILER, "check", self.package, "--diagnostics=json"], capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        diagnostic, = json.loads(result.stderr)
        self.assertEqual(diagnostic["code"], "LYT0007")
        self.assertIn("Outer", diagnostic["message"])

    def test_explicit_initializer_uses_the_same_binding_and_gc_rules(self):
        (self.package / "lyra.json5").write_text('{name:"test/objects", version:"0.1.0"}')
        models = self.src / "models"
        models.mkdir()
        (models / "box.ly").write_text('''
pub class Box<Item> {
    val value: Item
    pub val name: String
    var tag: String?

    pub init(value: Item, name: String = "default", early: Bool = false) {
        this.value = value
        tag = None
        if early { this.name = "early"; return }
        match name { "" => { this.name = "empty" }, _ => { this.name = name } }
        defer { tag = Some(this.name) }
    }

    pub fn read(): Item { value }
    pub fn label(): String { tag ?? name }
}
''', encoding="utf-8")
        self.run_source('''
import self.models.{Box as ItemBox}
class User(val name: String)
fn main(): Unit {
    val first = ItemBox(User("存活"))
    val alias = first
    val early = ItemBox(early: true, value: 5)
    val third: ItemBox<Int> = ItemBox(value: 7, name: "")
    gcCollect()
    print(alias.read().name)
    print(first.label())
    print(early.label())
    print(third.label())
}
''', "存活\ndefault\nearly\nempty\n".encode())

    def test_initialization_paths_and_escape_are_checked_statically(self):
        for body, message in (
            ('if condition { text = "maybe" }', "结束 init"),
            ('if condition { return }; this.text = "late"', "结束 init"),
            ('text = text', "尚未"),
            ('var text = "shadowed"; text = "local"', "结束 init"),
            ('publish(this); text = "late"', "使用 this"),
            ('read(); text = "late"', "使用 this"),
            ('val callback = () => { text }; text = "late"', "捕获 this"),
            ('if condition { text = "first" }; this.text = "second"', "只能初始化一次"),
            ('while condition { text = "loop" }; text = "done"', "不能在循环"),
            ('try { text = "try" } catch { this.text = "catch" }', "try 主体"),
        ):
            with self.subTest(body=body):
                self.main.write_text('class Item { val text: String; init(condition: Bool) { ' + body
                                     + ' }; fn read(): String { text } }\nfn publish(item: Item): Unit {}\nfn main(): Unit {}',
                                     encoding="utf-8")
                result = subprocess.run([COMPILER, "check", self.package, "--diagnostics=json"],
                                        capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                error, = json.loads(result.stderr)
                self.assertEqual(error["code"], "LYT0017")
                self.assertIn(message, error["message"])


if __name__ == "__main__":
    unittest.main()
