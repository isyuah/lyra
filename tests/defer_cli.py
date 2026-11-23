import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class Defers(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.package = Path(temp.name)
        (self.package / "src").mkdir()
        (self.package / "lyra.json5").write_text("{name:'test/cleanup',version:'1.0.0'}")
        self.source = self.package / "src/main.ly"

    def run_source(self, program, output):
        self.source.write_text(program, encoding="utf-8")
        for flags in ([], ["--release"]):
            with self.subTest(flags=flags):
                app = self.package / "app"
                build = subprocess.run([COMPILER, "build", self.package, *flags, "-o", app], capture_output=True, timeout=30)
                self.assertEqual(build.returncode, 0, build.stderr.decode())
                run = subprocess.run([app], capture_output=True, timeout=10)
                self.assertEqual((run.returncode, run.stdout.decode(), run.stderr), (0, output, b""))

    def test_lexical_lifo_return_propagation_break_and_continue(self):
        self.run_source('''
fn choose(flag: Bool): Int {
    var number = 1
    defer print(number)
    defer { number += 2 }
    {
        defer print(8)
        if flag { return number }
        number = 4
    }
    number
}
fn source(): Result<Int,String> { Err("bad") }
fn propagate(): Result<Int,String> {
    defer print(30)
    { defer print(31); val value = source()?; print(value) }
    Ok(1)
}
fn main(): Unit {
    print(choose(true))
    print(choose(false))
    print(match propagate() { Ok(value) => "unexpected", Err(reason) => reason })
    var index = 0
    while index < 5 {
        index += 1
        val item = index
        defer print(item)
        if item == 1 { continue }
        if item == 3 { break }
        print(20)
    }
    print(90)
}
''', "8\n3\n1\n8\n6\n4\n31\n30\nbad\n1\n20\n2\n3\n90\n")

    def test_worker_cleanup_closes_sqlite_before_repeated_await(self):
        self.run_source('''
import std.io.assert
import std.sqlite.open
class Label(val text: String)
async fn collect(): Unit { gcCollect() }
async fn started(): Unit { print(23) }
async fn quit(): Unit {
    defer print(24)
    spawn started();
}
async fn main(): Unit {
    {
        val label = Label("held" + " across GC")
        defer print(label.text)
        await collect()
    }
    val job = blocking {
        val connection = open(":memory:")
        defer assert(connection.close().ok(), "close connection")
        val query = connection.prepare("SELECT 73")
        defer assert(query.close().ok(), "finalize query")
        assert(query.next().row(), "row")
        print(query.readInt(0).value)
        connection
    }
    val closed = await job
    assert(!closed.prepare("SELECT 1").status().ok(), "defer did not close native handle")
    val alias = await job
    assert(alias.close().ok(), "idempotent close")
    await quit()
    print(25)
}
''', "held across GC\n73\n23\n24\n25\n")

    def test_cleanup_cannot_suspend(self):
        for program, effect in (
            ("async fn main(): Unit { defer { await main(); } }", "await"),
            ("async fn main(): Unit { defer { spawn main(); } }", "spawn"),
            ("fn stop(job: Task<Int>): Unit { job.cancel() }\nfn main(): Unit {}", "cancel"),
            ("async fn main(): Unit { val job = main(); defer job.cancel() }", "cancel"),
            ("async fn main(): Unit { val job = main(); await blocking { job.cancel() } }", "cancel"),
        ):
            self.source.write_text(program)
            run = subprocess.run([COMPILER, "check", self.package, "--diagnostics=json"], capture_output=True, timeout=10)
            self.assertNotEqual(run.returncode, 0)
            item, = json.loads(run.stderr)
            self.assertEqual((item["code"], item["range"]["start"]["offset"]), ("LYT0008", program.index(effect)))

    def test_explicit_cancel_runs_cleanup_and_does_not_cancel_siblings(self):
        self.run_source('''
async fn leaf(): Unit { print(99) }
async fn victim(): Unit {
    defer print(7)
    await leaf()
    print(98)
}
async fn cancelTask(target: Task<Unit>): Unit {
    target.cancel()
    target.cancel()
    print(6)
}
async fn sibling(): Unit { print(5) }
async fn main(): Unit {
    val neverStarted = leaf()
    neverStarted.cancel()
    spawn neverStarted;
    val child = spawn victim()
    spawn cancelTask(child);
    spawn sibling();
}
''', "6\n5\n7\n")


if __name__ == "__main__":
    unittest.main()
