import json
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest

from platform_cli import executable


COMPILER = Path(sys.argv.pop(1)).resolve()


class Generics(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.package = Path(temporary.name)
        (self.package / 'src').mkdir()
        self.source = self.package / 'src/main.ly'

    def program(self, code):
        self.source.write_text(textwrap.dedent(code).strip() + '\n')

    def cli(self, command, *flags):
        return subprocess.run([COMPILER, command, self.package, *flags], capture_output=True, text=True, timeout=25)

    def run_native(self, expected):
        for flags in ([], ['--release']):
            with self.subTest(flags=flags):
                output = executable(self.package / ('release' if flags else 'debug'))
                result = self.cli('build', *flags, '-o', output)
                self.assertEqual(result.returncode, 0, result.stderr)
                run = subprocess.run([output], capture_output=True, text=True, timeout=5)
                self.assertEqual((run.returncode, run.stdout, run.stderr), (0, expected, ''))

    def rejects(self, code, text=None, span=None):
        result = self.cli('check', '--diagnostics=json')
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        diag, = json.loads(result.stderr)
        self.assertEqual(diag['code'], code, diag)
        if text is not None:
            self.assertIn(text, diag['message'])
        if span is not None:
            source = self.source.read_bytes()
            actual = source[diag['range']['start']['offset']:diag['range']['end']['offset']].decode()
            self.assertEqual(actual, span)

    def test_inferred_explicit_nested_and_comparison(self):
        self.program('''
            fn identity<T>(item: T): T { item }
            fn relay<U>(item: U): U { val copy: U = identity<U>(item); identity(copy) }
            fn main(): Unit {
                print(identity(7))
                print(identity<String>("Ada"))
                if 7 < relay(8) { print(relay("yes")) }
            }
        ''')
        self.run_native('7\nAda\nyes\n')

    def test_transparent_type_aliases_cross_module(self):
        (self.package / 'lyra.json5').write_text('{name:"demo",version:"0.1.0"}')
        module = self.package / 'src/types'
        module.mkdir()
        (module / 'ids.ly').write_text('pub type UserId = Int\npub type UserIds = List<UserId>\n')
        self.program('''
            import self.types.UserId
            import self.types.UserIds
            fn identity(id: UserId): UserId { id }
            fn first(ids: UserIds): UserId { ids[0] }
            fn main(): Unit {
                val id: UserId = identity(7)
                val ids: UserIds = [id]
                print(first(ids))
            }
        ''')
        self.run_native('7\n')

    def test_type_alias_cycles_and_private_leaks_are_rejected(self):
        self.program('type Left = Right\ntype Right = Left\nfn main(): Unit {}')
        self.rejects('LYT0003', '循环')
        self.program('struct Hidden(val value: Int)\npub type Public = Hidden\nfn main(): Unit {}')
        self.rejects('LYN0003', '私有类型')
        self.program('enum Loop<T> { Next(Loop<List<T>>) }\nfn main(): Unit {}')
        self.rejects('LYG0004', '改变了类型实参')

    def test_two_parameters_named_arguments_and_nominal_aggregate(self):
        self.program('''
            struct Point(val x: Int)
            fn choose<T, U>(key: T, left: U, right: U): U { left }
            fn main(): Unit {
                val first = choose(key: "x", right: Point(9), left: Point(4))
                print(first.x)
            }
        ''')
        self.run_native('4\n')

    def test_structural_bound_can_use_builtin_collection_and_text_methods(self):
        self.program('''
            trait HasSize { fn size(): Int }
            fn empty<T: HasSize>(value: T): Bool { value.size() == 0 }
            fn count(value: dyn HasSize): Int { value.size() }
            fn main(): Unit {
                val values: List<Int> = []
                print(empty(values))
                print(empty([1, 2]))
                print(empty(""))
                print(empty("Lyra"))
                print(count("Hi" as dyn HasSize))
                print(count([1, 2, 3] as dyn HasSize))
            }
        ''')
        self.run_native('true\nfalse\ntrue\nfalse\n2\n3\n')

    def test_structural_bound_specializes_without_an_implements_clause(self):
        (self.package / 'lyra.json5').write_text('{name:"demo",version:"0.1.0"}')
        library = self.package / 'src/drawing'
        library.mkdir()
        (library / 'draw.ly').write_text('''pub trait Render { fn render(prefix: String): String }
pub fn draw<T: Render>(value: T): String { value.render(prefix: ">") }
pub fn relay<T: Render>(value: T): String { draw(value) }''')
        self.program('''import self.drawing.relay
            import self.drawing.Render as Printable
            struct Tag(val text: String) { pub fn render(prefix: String): String { prefix + text } }
            class Flag { pub fn render(prefix: String): String { prefix + "flag" } }
            fn localRender<T: Printable>(value: T): String { value.render("/") }
            fn main(): Unit { print(relay(Tag("Ada"))); print(relay<Flag>(Flag())); print(localRender(Flag())) }
        ''')
        self.run_native('>Ada\n>flag\n/flag\n')
        self.program(self.source.read_text().replace('pub fn render(prefix:', 'fn render(prefix:', 1))
        self.rejects('LYG0005', 'render', 'relay')

    def test_constraints_check_signatures_unused_bodies_and_type_arguments(self):
        header = 'trait Render { fn render(): String }\nfn draw<T: Render>(value: T): String { value.render() }\n'
        self.program(header + 'struct Wrong { fn render(): Int { 7 } }\nfn main(): Unit { print(draw(Wrong())) }')
        self.rejects('LYG0005', 'Int', 'draw')
        self.program(header + 'struct Wrong { static fn render(): String { "wrong" } }\nfn main(): Unit { print(draw(Wrong())) }')
        self.rejects('LYG0005', 'render', 'draw')
        self.program(header + 'fn unused<U>(value: U): String { draw(value) }\nfn main(): Unit {}')
        self.rejects('LYG0005', 'render', 'draw')
        self.program(header + 'fn unused<U: Render>(value: U): Int { value.render() }\nfn main(): Unit {}')
        self.rejects('LYT0001', 'String')
        self.program(header + 'fn unused<U: Missing>(value: U): U { value }\nfn main(): Unit {}')
        self.rejects('LYG0005', 'Missing', 'Missing')
        self.program(header + 'struct Wrong {}\nfn main(): Unit { val callback: (Wrong) -> String = draw }')
        self.rejects('LYG0005', 'render', 'draw')

    def test_multiple_structural_bounds_merge_forward_and_reject_conflicts(self):
        self.program('''
            trait Readable { fn read(): String }
            trait Labelled { fn label(): String }
            trait Renderable { fn render(): String }
            trait Printable { fn render(): String }
            struct Entry(val text: String) {
                fn read(): String { text }
                fn label(): String { "entry" }
                fn render(): String { text }
            }
            fn render<T: Readable & Labelled & Renderable & Printable>(value: T): String {
                value.label() + ":" + value.read() + ":" + value.render()
            }
            fn relay<T: Readable & Labelled & Renderable & Printable>(value: T): String { render(value) }
            fn main(): Unit { print(relay(Entry("Ada"))) }
        ''')
        self.run_native('entry:Ada:Ada\n')

        self.program('''
            trait AsText { fn render(): String }
            trait AsNumber { fn render(): Int }
            fn ambiguous<T: AsText & AsNumber>(value: T): String { value.render() }
            fn main(): Unit {}
        ''')
        self.rejects('LYG0006', '冲突', 'render')

        self.program('''
            trait Readable { fn read(): String }
            trait Labelled { fn label(): String }
            fn render<T: Readable & Labelled>(value: T): String { value.label() + value.read() }
            struct Incomplete { fn read(): String { "read" } }
            fn main(): Unit { print(render(Incomplete())) }
        ''')
        self.rejects('LYG0005', 'Labelled', 'render')

    def test_associated_types_specialize_across_modules(self):
        (self.package / 'lyra.json5').write_text('{name:"demo",version:"0.1.0"}')
        library = self.package / 'src/data'
        library.mkdir()
        (library / 'reader.ly').write_text('''
pub trait Reader { type Item; fn read(): Item }
pub struct TextReader(pub val text: String) {
    pub type Item = String
    pub fn read(): String { text }
}
pub struct NumberReader(pub val value: Int) {
    pub type Item = Int
    pub fn read(): Int { value }
}
pub fn readValue<T: Reader>(reader: T): T.Item { reader.read() }
pub fn pair<T: Reader>(reader: T, other: T.Item): List<T.Item> { [reader.read(), other] }
''')
        self.program('''
            import self.data.Reader as Readable
            import self.data.TextReader
            import self.data.NumberReader
            import self.data.readValue
            import self.data.pair
            class Box<Value>(pub val item: Value) {
                pub type Item = Value
                pub fn read(): Value { item }
            }
            fn readAlias<T: Readable>(reader: T) { readValue(reader) }
            fn main(): Unit {
                val text: String = readValue(TextReader("Ada"))
                val alias: String = readAlias(Box("Grace"))
                val names: List<String> = pair(TextReader("Ada"), "Lovelace")
                val numbers: List<Int> = pair(NumberReader(7), 8)
                print(text + ":" + alias + ":" + names[0] + ":" + names[1])
                print(numbers[0] + numbers[1])
            }
        ''')
        self.run_native('Ada:Grace:Ada:Lovelace\n15\n')

    def test_generic_extension_properties_across_modules(self):
        (self.package / 'lyra.json5').write_text('{name:"demo",version:"0.1.0"}')
        library = self.package / 'src/data'
        library.mkdir()
        (library / 'views.ly').write_text('''
pub extension ListView<T> for List<T> {
    val first: T get() { this[0] }
}
''')
        self.program('''
            import self.data.ListView
            struct Entry(val value: Int)
            extension EntryView for Entry {
                val value: Int get() { 999 }
            }
            fn make(): List<Int> { print("make"); [7, 8] }
            fn main(): Unit {
                print(make().first)
                print(Entry(3).value)
            }
        ''')
        self.run_native('make\n7\n3\n')

    def test_extension_property_rejects_write_and_ambiguous_groups(self):
        self.program('''
            struct Entry(val value: Int)
            extension EntryView for Entry { val doubled: Int get() { value * 2 } }
            fn main(): Unit { Entry(2).doubled = 4 }
        ''')
        self.rejects('LYT0004', '不能给此位置赋值', 'Entry(2).doubled')

        self.program('''
            struct Entry(val value: Int)
            extension FirstView for Entry { val label: String get() { "first" } }
            extension SecondView for Entry { val label: String get() { "second" } }
            fn main(): Unit { print(Entry(2).label) }
        ''')
        self.rejects('LYT0019', '扩展属性存在歧义', 'label')

        (self.package / 'lyra.json5').write_text('{name:"demo",version:"0.1.0"}')
        library = self.package / 'src/data'
        library.mkdir()
        (library / 'model.ly').write_text('''
pub struct Entry(val secret: Int)
pub extension EntryView for Entry { val secret: Int get() { 999 } }
pub fn makeEntry(): Entry { Entry(2) }
''')
        self.program('''
            import self.data.Entry
            import self.data.EntryView
            import self.data.makeEntry
            fn main(): Unit { print(makeEntry().secret) }
        ''')
        self.rejects('LYN0003', '模块私有', 'secret')

    def test_associated_types_reject_missing_private_cyclic_and_dynamic_implementations(self):
        (self.package / 'lyra.json5').write_text('{name:"demo",version:"0.1.0"}')
        library = self.package / 'src/data'
        library.mkdir(parents=True)
        (library / 'reader.ly').write_text('''pub trait Reader { type Item; fn read(): Item }
pub fn readValue<T: Reader>(reader: T): T.Item { reader.read() }''')
        self.program('''
            import self.data.readValue
            struct Missing { pub fn read(): String { "missing" } }
            fn main(): Unit { print(readValue(Missing())) }
        ''')
        self.rejects('LYG0005', 'Item', 'readValue')
        self.program('''
            import self.data.readValue
            struct Hidden { type Item = String; pub fn read(): String { "hidden" } }
            fn main(): Unit { print(readValue(Hidden())) }
        ''')
        self.rejects('LYG0005', 'Item', 'readValue')
        self.program('''
            import self.data.readValue
            struct Wrong { pub type Item = String; pub fn read(): Int { 0 } }
            fn main(): Unit { print(readValue(Wrong())) }
        ''')
        self.rejects('LYG0005', 'Int', 'readValue')
        self.program('''
            struct Loop { pub type Item = Loop.Item }
            fn main(): Unit {}
        ''')
        self.rejects('LYG0007', '循环', 'Loop.Item')
        self.program('''
            trait Reader { type Item; fn read(): Item }
            fn main(): Unit { val reader: dyn Reader = None }
        ''')
        self.rejects('LYG0007', '全部关联类型', 'dyn Reader')

    def test_bound_dynamic_interfaces_dispatch_and_preserve_type_identity(self):
        (self.package / 'lyra.json5').write_text('{name:"demo",version:"0.1.0"}')
        library = self.package / 'src/data'
        library.mkdir(parents=True)
        (library / 'reader.ly').write_text('''pub trait Reader { type Item; type Index; fn read(index: Index): Item }
pub type TextReader = dyn Reader<Index=Int, Item=String>
pub fn genericRead<T>(reader: dyn Reader<Item=T,Index=Int>): T { reader.read(0) }
''')
        self.program('''
            import self.data.Reader as Input
            import self.data.TextReader
            import self.data.genericRead
            trait View { type Item; fn read(index: Int): Item }
            struct Label<T>(val text: T) { pub type Item = T; pub type Index = Int64; pub fn read(index: Int): T { text } }
            class Words(val values: List<String>) { pub type Item = String; pub type Index = Int; pub fn read(index: Int): String { values[index] } }
            fn throughConstraint<T: Input>(reader: T, index: T.Index): T.Item { reader.read(index) }
            fn main(): Unit {
                val first = Label("label") as dyn Input<Item=String,Index=Int>
                val second = Words(["words"]) as dyn Input<Index=Int64,Item=String>
                val items: List<TextReader> = [first, second]
                for item in items { print(genericRead(item)) }
                print(throughConstraint(second, 0))
                print((first as dyn View<Item=String>).read(0))
                val box: Any = second
                print(box is TextReader)
                print(box is dyn Input<Item=Int,Index=Int>)
                print(match box as? TextReader { Some(item) => item.read(0), None => "wrong" })
            }
        ''')
        self.run_native('label\nwords\nwords\nlabel\ntrue\nfalse\nwords\n')

    def test_dynamic_associated_binding_rejections(self):
        header = '''trait Reader { type Item; fn read(): Item }
struct Number { pub type Item = Int; pub fn read(): Int { 7 } }
'''
        for body, code, message in (
            ('val item = Number() as dyn Reader<Item=String>', 'LYG0007', '需要 String'),
            ('val item = Number() as dyn Reader<Other=Int>', 'LYG0007', 'Other'),
            ('val item = Number() as dyn Reader<Item=Int,Item=String>', 'LYG0007', '重复绑定'),
        ):
            with self.subTest(body=body):
                self.program(header + 'fn main(): Unit { ' + body + ' }')
                self.rejects(code, message)
        self.program(header.replace('trait Reader', 'pub trait Reader') + 'struct Hidden()\npub type Leaking = dyn Reader<Item=Hidden>\nfn main(): Unit {}')
        self.rejects('LYN0003', 'Hidden')

    def test_async_structural_method_preserves_cold_task(self):
        self.program('''trait Fetch { async fn read(): Int }
            struct Store { async fn read(): Int { print("started"); 7 } }
            async fn fetch<T: Fetch>(store: T): Int { await store.read() }
            async fn main(): Unit { val task = fetch(Store()); print("cold"); print(await task) }''')
        self.run_native('cold\nstarted\n7\n')
        self.program(self.source.read_text().replace('struct Store { async fn', 'struct Store { fn'))
        self.rejects('LYG0005', 'async fn read', 'fetch')

    def test_recursive_instance_and_context_only_inference(self):
        self.program('''
            fn loop<T>(item: T, count: Int): T {
                if count == 0 { item } else { loop(item, count - 1) }
            }
            fn forever<T>(): T { forever<T>() }
            fn notCalled(): Int { forever() }
            fn main(): Unit { print(loop(17, 10)) }
        ''')
        self.run_native('17\n')

    def test_conflicting_missing_and_unknown_type_arguments(self):
        self.program('''
            fn choose<T>(left: T, right: T): T { left }
            fn main(): Unit { choose(7, "wrong"); }
        ''')
        self.rejects('LYT0001', span='"wrong"')
        self.program('''
            fn idle<T>(): Unit {}
            fn main(): Unit { idle() }
        ''')
        self.rejects('LYG0002', 'T', 'idle()')
        self.program('''
            fn left<T>(item: T): T { item }
            fn main(): Unit { val right = 2; print(left<right>(7)) }
        ''')
        self.rejects('LYT0003', 'right', 'right')

    def test_abstract_definition_checked_even_if_never_called(self):
        self.program('''
            fn notGenericEnough<T>(input: T): T { input + input }
            fn main(): Unit {}
        ''')
        self.rejects('LYT0001')
        self.program('''
            fn unused<T>(input: T): T { missing(input) }
            fn main(): Unit {}
        ''')
        self.rejects('LYN0001', 'missing', 'missing')

    def test_explicit_arguments_to_non_generic_and_wrong_arity(self):
        self.program('''
            fn routine(value: Int): Int { value }
            fn main(): Unit { print(routine<Int>(2)) }
        ''')
        self.rejects('LYG0001')
        self.program('''
            fn identity<T>(value: T): T { value }
            fn main(): Unit { print(identity<Int,Bool>(2)) }
        ''')
        self.rejects('LYG0001')

    def test_generic_values_project_types_and_match_fields(self):
        self.program('''
            struct Pair<First,Second>(var first: First, val second: Second)
            class Empty<Item>()
            fn swap<Left,Right>(pair: Pair<Left,Right>): Pair<Right,Left> {
                match pair { Pair(first: first, second: second) => Pair(second, first) }
            }
            fn seeded<Item>(items: List<Item>, value: Item): Item { value }
            fn main(): Unit {
                var pair = Pair(first: "value", second: 7)
                val copy = pair
                pair.first = "changed"
                val reversed = swap(copy)
                print(reversed.first)
                print(reversed.second)
                val empty: Empty<Int> = Empty()
                print(empty == Empty<Int>())
                print(seeded([], 9))
            }
        ''')
        self.run_native('7\nvalue\nfalse\n9\n')

    def test_generic_methods_use_receiver_types_and_infer_tail_lambdas(self):
        self.program('''
            trait Read { fn read(): String }
            class Cell<Item>(val item: Item, var next: Cell<Item>?) {
                fn read(): Item { item }
                fn map<Other>(transform: (Item) -> Other): Cell<Other> { Cell<Other>(transform(item), None) }
                async fn getNext(): Item? {
                    await blocking { gcCollect() }
                    match next { None => None, Some(node) => Some(node.item) }
                }
            }
            fn read<Reader: Read>(value: Reader): String { value.read() }
            async fn main(): Unit {
                val tail = Cell(next: None, item: "text")
                val node = Cell("head", Cell<String>("linked", None))
                val mapped = tail.map { it.size }
                print((await node.getNext()) ?? "missing")
                print(read(tail))
                print(mapped.read())
                print(mapped.map<Bool>() { it == 4 }.read())
            }
        ''')
        self.run_native('linked\ntext\n4\ntrue\n')

    def test_generic_aggregate_rejections(self):
        for source, code, text in (
            ('struct Pair<Item,Item>(val first: Item)', 'LYN0002', 'Item'),
            ('struct Bad<Item>(val item: Unknown)', 'LYT0003', 'Unknown'),
            ('struct Bad<Item>(val next: Bad<Item>?)', 'LYT0007', 'Bad'),
            ('class Bad<Item>(val next: Bad<List<Item>>?)', 'LYG0004', 'Bad'),
            ('struct Box<Item>(val item: Item)\nfn accept(item: Box): Unit {}', 'LYG0001', 'Box'),
            ('class Box<Item>(val item: Item) { fn bad(): String { item } }', 'LYT0001', 'Item'),
            ('struct Box<Item>(val item: Item)\nfn accept(value: Box<Int>): Unit {}\nfn wrong(): Unit { accept(Box("str")) }', 'LYT0001', 'Box<Int>'),
        ):
            with self.subTest(source=source):
                self.program(source + '\nfn main(): Unit {}')
                self.rejects(code, text)

    def test_nominal_constraints_cover_constructor_members_and_annotations(self):
        self.program('''
            trait Render { fn render(): String }
            class Name(val text: String) { fn render(): String { text } }
            class Card<T: Render>(val item: T, val label: String = item.render()) {
                fn read(): String { item.render() + ":" + label }
                static fn make(value: T): Card<T> { Card(value) }
            }
            enum Load<T: Render> { Ready(T), Wait }
            fn card<T: Render>(state: Load<T>): Card<T>? {
                match state { Ready(item) => Some(Card.make(item)), Wait => None }
            }
            fn main(): Unit {
                print(Card(Name("card")).read())
                print(card(Load.Ready(Name("load")))?.read() ?? "missing")
            }
        ''')
        self.run_native('card:card\nload:load\n')
        for body in (
            'fn main(): Unit { Card(7); }',
            'fn main(): Unit { val wait = Load<Int>.Wait }',
            'fn main(): Unit { val wait: Load<Int> = Load.Wait }',
            'fn illegal<T>(value: T): Card<T> { Card(value) }\nfn main(): Unit {}',
            'struct Wrapper(val state: Load<Int>)\nfn main(): Unit {}',
        ):
            with self.subTest(body=body):
                self.program('''trait Render { fn render(): String }
                    struct Card<T: Render>(val item: T)
                    enum Load<T: Render> { Ready(T), Wait }
                ''' + body)
                self.rejects('LYG0005', 'Render')


if __name__ == '__main__':
    unittest.main()
