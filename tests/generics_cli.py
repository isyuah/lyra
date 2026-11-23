import json
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest


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
                output = self.package / 'program'
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
        (library / 'draw.ly').write_text('''pub trait Render { fn render(self, prefix: String): String }
pub fn draw<T: Render>(value: T): String { value.render(prefix: ">") }
pub fn relay<T: Render>(value: T): String { draw(value) }''')
        self.program('''import self.drawing.relay
            import self.drawing.Render as Printable
            struct Tag(val text: String) { pub fn render(self, prefix: String): String { prefix + self.text } }
            class Flag { pub fn render(self, prefix: String): String { prefix + "flag" } }
            fn localRender<T: Printable>(value: T): String { value.render("/") }
            fn main(): Unit { print(relay(Tag("Ada"))); print(relay<Flag>(Flag())); print(localRender(Flag())) }
        ''')
        self.run_native('>Ada\n>flag\n/flag\n')
        self.program(self.source.read_text().replace('pub fn render(self, prefix:', 'fn render(self, prefix:', 1))
        self.rejects('LYG0005', 'render', 'relay')

    def test_constraints_check_signatures_unused_bodies_and_type_arguments(self):
        header = 'trait Render { fn render(self): String }\nfn draw<T: Render>(value: T): String { value.render() }\n'
        self.program(header + 'struct Wrong { fn render(self): Int { 7 } }\nfn main(): Unit { print(draw(Wrong())) }')
        self.rejects('LYG0005', 'Int', 'draw')
        self.program(header + 'fn unused<U>(value: U): String { draw(value) }\nfn main(): Unit {}')
        self.rejects('LYG0005', 'render', 'draw')
        self.program(header + 'fn unused<U: Render>(value: U): Int { value.render() }\nfn main(): Unit {}')
        self.rejects('LYT0001', 'String')
        self.program(header + 'fn unused<U: Missing>(value: U): U { value }\nfn main(): Unit {}')
        self.rejects('LYG0005', 'Missing', 'Missing')
        self.program(header + 'struct Wrong {}\nfn main(): Unit { val callback: (Wrong) -> String = draw }')
        self.rejects('LYG0005', 'render', 'draw')

    def test_async_structural_method_preserves_cold_task(self):
        self.program('''trait Fetch { async fn read(self): Int }
            struct Store { async fn read(self): Int { print("started"); 7 } }
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


if __name__ == '__main__':
    unittest.main()
