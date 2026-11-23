import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class Async(unittest.TestCase):
    def setUp(self):
        workspace = tempfile.TemporaryDirectory()
        self.addCleanup(workspace.cleanup)
        self.package = Path(workspace.name)
        directory = self.package / "src"
        directory.mkdir()
        self.main = directory / "main.ly"

    def run_source(self, source, expected):
        self.main.write_text(source, encoding="utf-8")
        for mode in ([], ["--release"]):
            with self.subTest(mode=mode):
                app = self.package / "app"
                built = subprocess.run([COMPILER, "build", self.package, *mode, "-o", app], capture_output=True, timeout=30)
                self.assertEqual(built.returncode, 0, built.stderr.decode())
                run = subprocess.run([app], capture_output=True, timeout=15)
                self.assertEqual((run.returncode, run.stdout.decode(), run.stderr), (0, expected, b""))

    def test_cold_arguments_repeated_await_and_spawn(self):
        self.run_source("""
fn mark(value: Int): Int { print(value); value }
async fn work(value: Int): Int { print(20); value + 1 }
async fn main(): Unit {
    val unused = work(mark(8))
    val cold = work(mark(10))
    print(11)
    print(await cold)
    print(await cold)
    val running = spawn work(30)
    print(40)
    print(await running)
    spawn work(50);
}
""", "8\n10\n11\n20\n11\n11\n40\n20\n31\n20\n")

    def test_gc_in_child_and_values_live_across_await(self):
        self.run_source("""
class Cell(var number: Int)
struct Counter(var value: Int)
fn sum(first: Int, second: Int): Int { first + second }
async fn value(number: Int): Int { gcCollect(); number }
async fn make(): Cell { gcCollect(); Cell(63) }
async fn counter(): Counter { Counter(12) }
async fn main(): Unit {
    val cell = Cell(1)
    cell.number += await value(3)
    print(sum(cell.number, await value(5)))
    val held = make()
    val original = await held
    original.number = 7
    print((await held).number)
    val heldCounter = counter()
    var copy = await heldCounter
    copy.value += 1
    print(copy.value)
    print((await heldCounter).value)
    var count = 0
    while count < 3 {
        val next = if count == 1 { await value(2) } else { await value(1) }
        count += next
    }
    print(count)
}
""", "9\n7\n13\n12\n3\n")

    def test_deep_await_chain_returns_without_native_stack_recursion(self):
        self.run_source("""
async fn depth(count: Int): Int {
    if count == 0 { 0 } else { 1 + await depth(count - 1) }
}
async fn main(): Unit { print(await depth(5000)) }
""", "5000\n")

    def test_invalid_task_operations(self):
        for source, marked in (("fn main(): Unit { val bad = await work() } async fn work(): Unit {}", "await"),
                               ("fn main(): Unit { val bad = spawn work() } async fn work(): Unit {}", "spawn"),
                               ("async fn main(): Unit { val bad = await 4 }", "4"),
                               ("async fn main(): Unit { val bad = spawn 5 }", "5")):
            with self.subTest(source=source):
                self.main.write_text(source)
                result = subprocess.run([COMPILER, "check", self.package, "--diagnostics=json"], capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                error, = json.loads(result.stderr)
                where = error["range"]
                self.assertEqual((error["code"], where["start"]["offset"], where["end"]["offset"]),
                                 ("LYT0008", source.index(marked), source.index(marked) + len(marked)))


if __name__ == "__main__":
    unittest.main()
