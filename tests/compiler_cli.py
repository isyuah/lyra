import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()
FIXTURES = Path(__file__).resolve().parents[1] / "examples" / "compiler-smoke"


class PackageTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.package = Path(temporary.name) / "package with spaces"
        self.src = self.package / "src"
        self.src.mkdir(parents=True)
        self.main = self.src / "main.ly"

    def cli(self, command, *options, env=None):
        return subprocess.run([COMPILER, command, self.package, *options],
                              capture_output=True, text=True, env=env, timeout=15)

    def fixture(self, kind):
        shutil.copyfile(FIXTURES / kind / "src" / "main.ly", self.main)

    def semicolon_error(self, result):
        self.assertNotEqual(result.returncode, 0)
        diagnostic, = json.loads(result.stderr)
        self.assertEqual(diagnostic["code"], "LYT0001")
        self.assertEqual((diagnostic["expected"], diagnostic["actual"]), ("Int", "Unit"))
        self.assertEqual(Path(diagnostic["file"]).name, "main.ly")
        data = self.main.read_bytes()
        start = diagnostic["range"]["start"]
        end = diagnostic["range"]["end"]
        self.assertEqual((start["offset"], end["offset"]), (data.index(b";"), data.index(b";") + 1))
        prefix = data[:start["offset"]].decode()
        self.assertEqual((start["line"], start["column"]),
                         (prefix.count("\n") + 1, len(prefix.rsplit("\n", 1)[-1]) + 1))
        self.assertTrue(diagnostic["hint"])
        return diagnostic


class Frontend(PackageTest):
    def test_cc001_check(self):
        self.fixture("fail")
        diagnostic = self.semicolon_error(self.cli("check", "--diagnostics=json"))
        text = self.cli("check")
        self.assertNotEqual(text.returncode, 0)
        self.assertIn("LYT0001", text.stderr)
        self.assertIn("Int", text.stderr)
        self.assertIn("Unit", text.stderr)
        start = diagnostic["range"]["start"]
        self.assertIn(f'{start["line"]}:{start["column"]}', text.stderr)
        self.assertIn(diagnostic["hint"], text.stderr)

    def test_cc002_check_without_clang(self):
        self.fixture("pass")
        result = self.cli("check", env={**os.environ, "PATH": ""})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.package / "build").exists())

    def test_crlf_and_utf8_location(self):
        self.main.write_bytes("// 注释\r\nfn total(): Int { /* 加法 */ 1 + 2; }\r\n"
                              "fn main(): Unit { print(total()) }\r\n".encode())
        self.semicolon_error(self.cli("check", "--diagnostics=json"))

    def test_check_rejects_bad_programs(self):
        for source in ("fn main(): Unit { print(missing()) }", "fn main(): Unit { print() }",
                       "fn main(): Unit { print(1, 2) }", "fn main(): Unit { print(print(1)) }",
                       "fn main(): Unit { print(9223372036854775808) }", "fn main(): Int { 1 }",
                       "fn f(): Int { 1 }", "fn main(): Unit {} fn main(): Unit {}",
                       "fn main(): Unit { print(1 }", "fn main(): Unit { 1\n+ 2; }"):
            with self.subTest(source=source):
                self.main.write_text(source)
                result = self.cli("check", "--diagnostics=json")
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(json.loads(result.stderr)[0]["code"])


class Native(PackageTest):
    def test_cc001_no_output(self):
        self.fixture("fail")
        output = self.package / "out"
        self.semicolon_error(self.cli("build", "--diagnostics=json", "-o", output,
                                      env={**os.environ, "PATH": ""}))
        self.assertFalse(output.exists())
        output.write_bytes(b"previous artifact")
        self.semicolon_error(self.cli("build", "--diagnostics=json", "-o", output))
        self.assertEqual(output.read_bytes(), b"previous artifact")

    def test_cc002_executable(self):
        self.fixture("pass")
        for profile, flags in (("debug", []), ("release", ["--release"])):
            with self.subTest(profile=profile):
                result = self.cli("build", *flags)
                self.assertEqual(result.returncode, 0, result.stderr)
                output = self.package / "build" / profile / "app"
                run = subprocess.run([output], capture_output=True, timeout=5)
                self.assertEqual((run.returncode, run.stdout, run.stderr), (0, b"3\n", b""))
                dynamic = subprocess.check_output(["readelf", "-dW", output], text=True)
                needed = [line.split("[")[1].split("]")[0] for line in dynamic.splitlines() if "(NEEDED)" in line]
                self.assertEqual(needed, ["libc.so.6"])

    def test_same_module_forward_calls_and_block_values(self):
        self.main.write_text("fn main(): Unit { print(answer()) }\n")
        (self.src / "z.ly").write_text("fn answer(): Int { print(1);\n (40 +\n 2)\n }\n")
        nested = self.src / "other"
        nested.mkdir()
        (nested / "bad.ly").write_text("not in this module")
        output = self.package / "new folder" / "my app"
        result = self.cli("build", "-o", output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(subprocess.check_output([output], timeout=5), b"1\n42\n")

    def test_overflow_checked_in_both_profiles(self):
        for value, delta in (("9223372036854775807", "1"), ("-9223372036854775808", "-1")):
            self.main.write_text(f"fn limit(): Int {{ {value} }}\nfn main(): Unit {{ print(limit() + {delta}) }}")
            for flags in ([], ["--release"]):
                with self.subTest(value=value, flags=flags):
                    output = self.package / "overflow"
                    result = self.cli("build", "-o", output, *flags)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    run = subprocess.run([output], capture_output=True, text=True, timeout=5)
                    self.assertNotEqual(run.returncode, 0)
                    self.assertEqual(run.stdout, "")
                    self.assertIn("overflow", run.stderr.lower())
                    self.assertIn("main.ly:2:", run.stderr)

    def test_failed_tool_does_not_replace_artifact(self):
        self.fixture("pass")
        output = self.package / "existing"
        output.write_bytes(b"previous artifact")
        result = self.cli("build", "--diagnostics=json", "-o", output, env={**os.environ, "PATH": ""})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("clang-18", json.loads(result.stderr)[0]["message"])
        self.assertEqual(output.read_bytes(), b"previous artifact")

    def test_link_failure_preserves_old_output(self):
        self.fixture("pass")
        toolchain = self.package / "toolchain"
        (toolchain / "bin").mkdir(parents=True)
        (toolchain / "lib").mkdir()
        compiler = toolchain / "bin" / "lyra"
        shutil.copy2(COMPILER, compiler)
        (toolchain / "lib" / "liblyraRuntime.a").write_bytes(b"not a runtime archive")
        output = self.package / "existing"
        output.write_bytes(b"previous artifact")
        before = set(self.package.iterdir())
        result = subprocess.run([compiler, "build", self.package, "--diagnostics=json", "-o", output],
                                capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stderr)[0]["code"], "LYB0002")
        self.assertEqual(output.read_bytes(), b"previous artifact")
        self.assertEqual(set(self.package.iterdir()), before)


if __name__ == "__main__":
    unittest.main()
