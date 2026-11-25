import ctypes
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

from platform_cli import WINDOWS, executable, running


COMPILER = Path(sys.argv.pop(1)).resolve()


class OsTime(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / 'src').mkdir()
        (self.root / 'lyra.json5').write_text('{name:"test/os-time",version:"0.1.0"}', encoding='utf-8')

    def build(self, source, *flags):
        (self.root / 'src/main.ly').write_text(source, encoding='utf-8')
        app = executable(self.root / 'program')
        done = subprocess.run([COMPILER, 'build', self.root, *flags, '-o', app], capture_output=True, timeout=35)
        self.assertEqual(done.returncode, 0, done.stderr.decode())
        return app

    def test_exit_codes_and_assertion_source_with_cleanup(self):
        for entry in ('fn', 'async fn'):
            for code in (73, 256):
                with self.subTest(entry=entry, code=code):
                    app = self.build(f'{entry} main(): Int {{ defer print("cleaned"); {code} }}')
                    result = subprocess.run([app], capture_output=True, timeout=8)
                    self.assertEqual(result.stdout, b'cleaned\n')
                    self.assertEqual(result.returncode, 73 if code == 73 else 1)
                    if code == 256:
                        self.assertIn(b'expected an exit code in 0..255', result.stderr)
        app = self.build('''fn main(): Unit {
    var calls = 0
    defer print(calls)
    #assert({ calls += 1; true }, { calls += 100; "unused" })
    #assert({ calls += 1; false }, { calls += 10; "failure detail" })
}
''', '--release')
        result = subprocess.run([app], capture_output=True, timeout=8)
        self.assertEqual((result.returncode, result.stdout), (1, b'12\n'))
        self.assertIn(b'assertion failed: failure detail', result.stderr)
        self.assertIn(b'main.ly:5:6', result.stderr)

    def test_clock_calendar_and_explicit_offset(self):
        app = self.build('''
import std.time.Clock
import std.time.Date
import std.time.DateTime
import std.time.TimeError

fn date(value: Result<Date,TimeError>): Date {
    match value { Ok(parsed) => parsed, Err(_) => throw Exception("expected date") }
}
fn time(value: Result<DateTime,TimeError>): DateTime {
    match value { Ok(parsed) => parsed, Err(_) => throw Exception("expected timestamp") }
}

async fn main(): Unit {
    val leap = date(Date.parse("2024-02-29"))
    #assert(leap.year == 2024 && leap.month == 2 && leap.day == 29)
    #assert(match Date.parse("2026-02-29") { Err(InvalidDate) => true, _ => false })
    #assert(match DateTime.parse("2026-12-31T23:59:59") { Err(InvalidDateTime) => true, _ => false })
    val stamp = time(DateTime.parse("1970-01-01T01:00:00.123456789+01:00"))
    #assert(stamp.unixSeconds == 0 && stamp.unixMs() == 123 && stamp.nanosecond == 123456789 && stamp.offsetMinutes == 60)
    #assert(stamp.formatUtc() == "1970-01-01T00:00:00.123456789Z")
    #assert(stamp.toString() == "1970-01-01T01:00:00.123456789+01:00")
    #assert(stamp.date().toString() == "1970-01-01")
    val start = Clock.monotonicMs()
    await sleepMs(25)
    #assert(Clock.monotonicMs() > start)
    print(Clock.now().unixMs())
}
''')
        before = time.time_ns() // 1_000_000
        result = subprocess.run([app], capture_output=True, timeout=8)
        after = time.time_ns() // 1_000_000
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertLessEqual(before, int(result.stdout))
        self.assertLessEqual(int(result.stdout), after)

    def test_process_arguments_pipes_failure_and_limits(self):
        child = self.root / '子进程 🙂.py'
        child.write_text('''import os, sys
if sys.argv[1] == 'wait':
    import time
    time.sleep(60)
assert os.getcwd() == sys.argv[4], (os.getcwd(), sys.argv[4])
assert sys.argv[2:4] == ['', '中🙂 ;$&'], sys.argv
sys.stdout.buffer.write(b'x' * 70000)
sys.stderr.buffer.write(b'y' * 80000)
sys.exit(19)
''', encoding='utf-8')
        app = self.build('''
import std.os.ProcessOptions
import std.os.args
import std.os.runProcess

async fn main(): Unit {
    val params = match args() { Ok(value) => value, Err(error) => throw Exception(error.message()) }
    val argv = [params[0], params[1], "pipes", "", "中🙂 ;$&", params[2]]
    val cold = runProcess(argv, ProcessOptions(cwd: Some(params[2])))
    match await cold {
        Ok(child) => {
            #assert(child.exitCode == 19 && child.signal == None)
            #assert(child.stdout.size == 70000 && child.stdout[69999] == 120)
            #assert(child.stderr.size == 80000 && child.stderr[79999] == 121)
        },
        Err(_) => throw Exception("expected nonzero child result"),
    }
    #assert(match await runProcess(argv, ProcessOptions(maxOutputBytes: 65536, cwd: Some(params[2]))) { Err(OutputLimit) => true, _ => false })
    #assert(match await runProcess([params[0], params[1], "wait"], ProcessOptions(timeoutMs: 150)) { Err(Timeout) => true, _ => false })
    #assert(match await runProcess([]) { Err(Invalid) => true, _ => false })
    #assert(match await runProcess([params[2] + "/not-a-program"]) { Err(System(_)) => true, _ => false })
}
''')
        cwd = self.root / '工作 目录'
        cwd.mkdir()
        done = subprocess.run([app, sys.executable, child, cwd], capture_output=True, timeout=12)
        self.assertEqual((done.returncode, done.stdout), (0, b''), done.stderr.decode())

    def test_cancel_reaps_child_before_task_finishes(self):
        child = self.root / 'pending.py'
        child.write_text('''import os, pathlib, sys, time
marker = pathlib.Path(sys.argv[1])
temporary = marker.with_suffix('.tmp')
temporary.write_text(str(os.getpid()))
temporary.replace(marker)
time.sleep(60)
''', encoding='utf-8')
        app = self.build('''
import std.io.readLine
import std.os.args
import std.os.runProcess

async fn main(): Unit {
    val values = match args() { Ok(value) => value, Err(error) => throw Exception(error.message()) }
    val pending = spawn runProcess(values)
    val signal = await blocking { readLine() }
    #assert(match signal { Ok(Some("cancel")) => true, _ => false })
    pending.cancel()
    #assert(match awaitOutcome(pending) { Cancelled(_) => true, _ => false })
    print("reaped")
}
''')
        marker = self.root / 'pid'
        with running([app, sys.executable, child, marker], env={**os.environ, 'UV_THREADPOOL_SIZE': '2'}) as (process, output):
            deadline = time.monotonic() + 5
            while not marker.exists() and process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(marker.exists(), 'child did not start')
            pid = int(marker.read_text())
            if WINDOWS:
                kernel = ctypes.WinDLL('kernel32', use_last_error=True)
                kernel.OpenProcess.restype = ctypes.c_void_p
                kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
                kernel.CloseHandle.argtypes = [ctypes.c_void_p]
                handle = kernel.OpenProcess(0x00100000, False, pid)
                self.assertTrue(handle, ctypes.get_last_error())
                self.addCleanup(kernel.CloseHandle, handle)
            process.stdin.write(b'cancel\n')
            process.stdin.flush()
            self.assertEqual(output.line(timeout=5), b'reaped\n')
            if WINDOWS:
                self.assertEqual(kernel.WaitForSingleObject(handle, 0), 0)
            else:
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)
            self.assertEqual(process.wait(timeout=5), 0, process.stderr.read().decode())

    def test_process_timeout_releases_queued_work_without_starting_the_child(self):
        marker = self.root / 'never-started'
        app = self.build('''
import std.io.readLine
import std.os.ProcessOptions
import std.os.args
import std.os.runProcess
import std.sync.AtomicInt

async fn main(): Unit {
    val entered = AtomicInt()
    val parked = spawn blocking {
        entered.store(1)
        val _ = readLine()
    }
    while entered.load() == 0 { await sleepMs(1) }
    val argv = match args() { Ok(values) => values, Err(error) => throw Exception(error.message()) }
    #assert(match await runProcess(argv, ProcessOptions(timeoutMs: 100)) { Err(Timeout) => true, _ => false })
    print("timed out while queued")
    await parked
}
''')
        with running([app, sys.executable, '-c', 'import pathlib, sys; pathlib.Path(sys.argv[1]).touch()', marker],
                     env={**os.environ, 'UV_THREADPOOL_SIZE': '1'}) as (process, output):
            self.assertEqual(output.line(timeout=5), b'timed out while queued\n')
            process.stdin.write(b'release\n')
            process.stdin.flush()
            self.assertEqual(process.wait(timeout=5), 0, process.stderr.read().decode())
            self.assertFalse(marker.exists())


if __name__ == '__main__':
    unittest.main()
