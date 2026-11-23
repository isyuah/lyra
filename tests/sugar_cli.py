import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class Sugar(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.package = Path(temporary.name)
        (self.package / 'src').mkdir()
        self.main = self.package / 'src/main.ly'

    def test_list_loops_nullable_fields_methods_and_short_circuit(self):
        self.main.write_text('''
class Meter(var calls: Int) {
    fn tick(self): Int { self.calls += 1; self.calls }
}
struct Item(val value: Int?, val nested: Option<Int?>) {
    fn sum(self, extra: Int): Int { (self.value ?? 4) + extra }
}
fn maybe(counter: Meter, present: Bool): Item? {
    counter.tick()
    if present { Some(Item(None, Some(None))) } else { None }
}
fn numbers(counter: Meter): List<Int> { counter.tick(); [1, 2, 3, 4] }
fn identity<T>(items: List<T>, end: T): T { for item in items { return item }; end }
fn main(): Unit {
    val counter = Meter(0)
    val skip = maybe(counter, false)?.sum(counter.tick()) ?? 7
    print(skip)
    val result = maybe(counter, true)
        ?.sum(counter.tick()) ?? counter.tick()
    print(result)
    print(counter.calls)
    val nested = maybe(counter, true)?.nested
    print(match nested { Some(None) => 8, Some(Some(value)) => value, None => 9 })
    val preserved: Int? = nested ?? Some(19)
    print(preserved ?? 6)
    val missing: Int? = maybe(counter, true)?.value
    print(missing ?? None ?? 5)
    val condition: Bool? = Some(false)
    if condition ?? false || true { print(99) } else { print(11) }
    val values = numbers(counter)
    for item in values {
        values.add(90)
        if item == 2 { continue }
        if item == 4 { break }
        print(item)
    }
    print(values.size)
    for item in numbers(counter) { if item == 1 { break } }
    print(counter.calls)
    print(identity(["text"], "unseen"))
    val empty: List<Int> = []
    for item in empty { print(99) }
    val absent: Item? = None
    absent?.sum(counter.tick());
    val direct = Some(None) ?? Some(counter.tick())
    val typed: Int? = Some(None) ?? Some(counter.tick())
    print((direct ?? 0) + (typed ?? 0) + counter.calls)
    val changed = [1, 2]
    for item in changed {
        if item == 1 { changed[1] = 42 }
        print(item)
    }
}
''', encoding='utf-8')
        for flags in ([], ['--release']):
            with self.subTest(flags=flags):
                built = subprocess.run([COMPILER, 'build', self.package, *flags, '-o', self.package / 'app'], capture_output=True, timeout=30)
                self.assertEqual(built.returncode, 0, built.stderr.decode())
                ran = subprocess.run([self.package / 'app'], capture_output=True, timeout=5)
                self.assertEqual((ran.returncode, ran.stdout, ran.stderr), (0, b'7\n7\n3\n8\n6\n5\n11\n1\n3\n8\n7\ntext\n7\n1\n42\n', b''))

    def test_wrong_operands_report_source_sites(self):
        for code, expression, token in [('LYT0010', '1?.size', '?.'), ('LYT0010', '1 ?? 2', '??'),
                                        ('LYT0001', 'Some(3) ?? "wrong"', '"wrong"'),
                                        ('LYT0004', 'var value = 1; (None ?? value) = 2', '??'),
                                        ('LYT0014', 'for value in 7 {}', '7'),
                                        ('LYT0004', 'for value in [3] { value = 2 }', 'value'),
                                        ('LYT0001', 'val value: Option<Int?> = Some(None); val flat: Int = value ?? 7', '??')]:
            with self.subTest(expression=expression):
                self.main.write_text('fn main(): Unit { ' + expression + '; }', encoding='utf-8')
                rejected = subprocess.run([COMPILER, 'check', self.package, '--diagnostics=json'], capture_output=True, timeout=10)
                self.assertNotEqual(rejected.returncode, 0)
                issue, = json.loads(rejected.stderr)
                self.assertEqual(issue['code'], code)
                where = issue['range']
                source = self.main.read_text(encoding='utf-8')
                self.assertIn(token, source[where['start']['offset']:where['end']['offset']])


if __name__ == '__main__':
    unittest.main()
