import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class TaskContext(unittest.TestCase):
    def test_activation_scopes_worker_and_waiter_deadline(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'src').mkdir()
            (root / 'lyra.json5').write_text("{name:'test/context',version:'1.0.0'}")
            (root / 'src/main.ly').write_text('''
import std.io.assert
import std.io.readFile
import std.io.writeFile
import std.task.trace
import std.task.remainingMs
import std.task.withTrace
import std.task.withTimeout

async fn inspect(): String {
    val first = trace()
    val worker = await blocking { gcCollect(); trace() }
    assert(trace() == first && worker == first, "worker or resumption lost trace")
    first
}
async fn shouldNotStart(): Unit { print("expired body ran") }
async fn deadlineGroup(live: Task<String>): Unit {
    spawn withTimeout(2, live);
    spawn withTimeout(0, shouldNotStart());
}
async fn main(): Unit {
    val cold = inspect()
    assert(trace() == "" && remainingMs() == -1, "root context")
    assert(await withTrace("B", cold) == "B", "context at activation")
    assert(trace() == "" && await withTrace("C", cold) == "B", "waiter changed terminal result")
    val live = spawn withTrace("owner", blocking {
        val signal = readFile("gate")
        assert(signal.ok(), "read gate")
        trace()
    })
    await deadlineGroup(live)
    assert(writeFile("gate", "release") == 0, "release gate")
    assert(await live == "owner", "waiter changed ownership")
    print("done")
}
''')
            os.mkfifo(root / 'gate')
            for mode in ([], ['--release']):
                with self.subTest(mode=mode):
                    built = subprocess.run([COMPILER, 'build', root, *mode, '-o', root / 'app'], capture_output=True, timeout=30)
                    self.assertEqual(built.returncode, 0, built.stderr.decode())
                    ran = subprocess.run([root / 'app'], cwd=root, env={**os.environ, 'UV_THREADPOOL_SIZE': '1'}, capture_output=True, timeout=10)
                    self.assertEqual((ran.returncode, ran.stdout, ran.stderr), (0, b'done\n', b''))


if __name__ == '__main__':
    unittest.main()
