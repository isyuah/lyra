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
