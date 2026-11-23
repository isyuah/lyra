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
    fn label(self): String { self.name }
    fn rename(self, name: String): Unit { self.name = name }
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

    def test_heap_roots_strings_and_inline_data(self):
        self.run_source(GC_FIXTURE.read_text(encoding="utf-8"), "持有\nA\0B\n1\n2\n".encode())

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

    def test_static_rejections(self):
        types = "struct Point(var x: Int)\nclass Box(val point: Point, var value: Int)\n"
        for body, code in (("val p = Point(1); p.x = 2", "LYT0004"),
                           ("val b = Box(Point(1), 2); b.point.x = 3", "LYT0004"),
                           ("val b = Box(Point(1), 2); b.point = Point(3)", "LYT0004"),
                           ("Point(x: true)", "LYT0001"),
                           ("Point(other: 1)", "LYT0002"),
                           ("val p = Point(1); p.missing", "LYN0001"),
                           ("val p = Point(1); val same = p == p", "LYT0006")):
            with self.subTest(body=body):
                self.main.write_text(types + "fn main(): Unit { " + body + "; }", encoding="utf-8")
                result = subprocess.run([COMPILER, "check", self.package, "--diagnostics=json"],
                                        capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                diagnostic, = json.loads(result.stderr)
                self.assertEqual(diagnostic["code"], code)
                self.assertEqual(diagnostic["range"]["start"]["line"], 3)

    def test_inline_layout_cycle_rejected(self):
        self.main.write_text("struct Outer(val inner: Inner)\nstruct Inner(val outer: Outer)\nfn main(): Unit {}")
        result = subprocess.run([COMPILER, "check", self.package, "--diagnostics=json"], capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        diagnostic, = json.loads(result.stderr)
        self.assertEqual(diagnostic["code"], "LYT0007")
        self.assertIn("Outer", diagnostic["message"])


if __name__ == "__main__":
    unittest.main()
