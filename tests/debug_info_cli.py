from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()
DEBUG_TOOL = Path(sys.argv.pop(1)).resolve()
WINDOWS = COMPILER.suffix.lower() == ".exe"


class DebugInfoTest(unittest.TestCase):
    def symbols_for(self, executable):
        codeview = executable.read_bytes().split(b"RSDS", 1)[1]
        name = codeview[20:].split(b"\x00", 1)[0].decode("utf-8")
        self.assertEqual(name, Path(name).name)
        self.assertRegex(name, re.escape(executable.stem) + r"\.[0-9a-fA-F]{16}\.pdb$")
        return executable.parent / name

    def inspect(self, executable, *, types=False):
        if WINDOWS:
            symbols = self.symbols_for(executable)
            self.assertTrue(symbols.is_file())
            command = [DEBUG_TOOL, "dump", "-l", "--files", "--symbols"]
            if types:
                command.append("--types")
            command.append(symbols)
        else:
            command = [DEBUG_TOOL, "--debug-info", "--debug-line", executable]
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_debug_artifact_contains_lyra_source_locations(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / "debug package"
            source = package / "src" / "main.ly"
            source.parent.mkdir(parents=True)
            source.write_text(
                "fn compute(value: Int): Int {\n"
                "    val result = value + 1\n"
                "    result\n"
                "}\n"
                "fn main(): Unit { print(compute(41)) }\n",
                encoding="utf-8",
            )

            built = subprocess.run([COMPILER, "build", package], capture_output=True, text=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            executable = package / "build" / "debug" / ("app.exe" if WINDOWS else "app")
            cache = package / "build" / "debug" / ".lyra-cache" / "objects"
            entries = list(cache.iterdir())
            self.assertEqual(len(entries), 1)
            old_time = 1_000_000_000
            os.utime(entries[0], ns=(old_time, old_time))
            rebuilt = subprocess.run([COMPILER, "build", package], capture_output=True, text=True, timeout=30)
            self.assertEqual(rebuilt.returncode, 0, rebuilt.stderr)
            self.assertEqual(list(cache.iterdir()), entries)
            self.assertEqual(entries[0].stat().st_mtime_ns, old_time)
            executed = subprocess.run([executable], capture_output=True, text=True, timeout=10)
            self.assertEqual((executed.returncode, executed.stdout), (0, "42\n"))

            source.write_text(source.read_text(encoding="utf-8").replace("value + 1", "value + 2"), encoding="utf-8")
            changed = subprocess.run([COMPILER, "build", package], capture_output=True, text=True, timeout=30)
            self.assertEqual(changed.returncode, 0, changed.stderr)
            self.assertEqual(len(list(cache.iterdir())), 2)
            executed = subprocess.run([executable], capture_output=True, text=True, timeout=10)
            self.assertEqual((executed.returncode, executed.stdout), (0, "43\n"))

            symbols = self.inspect(executable)
            self.assertIn("lyra.fn.compute", symbols)
            if WINDOWS:
                for fragment in ("main.ly", "`value`", "`result`"):
                    self.assertIn(fragment, symbols)
                lines = symbols.split('line/column/addr entries = ', 1)[1].split('\n\n', 1)[0]
                self.assertRegex(lines, r'\b2 [0-9A-Fa-f]{8} !')
            else:
                for fragment in ("DW_TAG_subprogram", 'name: "main.ly"', "      2 ", 'DW_AT_name\t("value")', 'DW_AT_name\t("result")'):
                    self.assertIn(fragment, symbols)

    def test_compound_fields_and_relocatable_release_symbols(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary)
            source = package / "src" / "main.ly"
            source.parent.mkdir()
            source.write_text(
                "class DebugNode(val nodeCount: Int, val nextNode: DebugNode?)\n"
                "struct DebugEnvelope(val prefixByte: UInt8, val payloadNode: DebugNode)\n"
                "enum DebugLoad { Pending, Ready(packet: DebugEnvelope), Rejected(message: String) }\n"
                "fn read(load: DebugLoad): Int {\n"
                "    match load { Pending => 0, Rejected(_) => -1, Ready(record) => record.payloadNode.nodeCount }\n"
                "}\n"
                "fn unwrap(value: Result<DebugLoad, Int>): Int { match value { Ok(load) => read(load), Err(code) => code } }\n"
                "fn main(): Unit {\n"
                "    val leaf = DebugNode(42, None)\n"
                "    val storage = DebugEnvelope(3, DebugNode(leaf.nodeCount, Some(leaf)))\n"
                "    print(unwrap(Ok(DebugLoad.Ready(storage))))\n"
                "}\n",
                encoding="utf-8",
            )
            for profile, options in (("debug", []), ("release", ["--release", "--debug-info"])):
                with self.subTest(profile=profile):
                    name = "custom name.exe" if WINDOWS else "custom name"
                    executable = package / profile / name
                    build = subprocess.run([COMPILER, "build", package, *options, "-o", executable], capture_output=True, text=True, timeout=40)
                    self.assertEqual(build.returncode, 0, build.stderr)
                    destination = package / ("moved " + profile)
                    executable.parent.rename(destination)
                    executable = destination / name
                    result = subprocess.run([executable], capture_output=True, text=True, timeout=10)
                    self.assertEqual((result.returncode, result.stdout, result.stderr), (0, "42\n", ""))
                    symbols = self.inspect(executable, types=True)
                    for fragment in ("DebugNode", "DebugEnvelope", "DebugLoad", "nodeCount", "nextNode", "prefixByte", "payloadNode", "packet", "hasValue", "isOk"):
                        self.assertTrue(fragment in symbols, f"{fragment} missing from {profile} symbols")
                    if WINDOWS:
                        field = next(line for line in symbols.splitlines() if "LF_MEMBER" in line and "`payloadNode`" in line)
                        self.assertRegex(field, r"offset = 8\b")
                    else:
                        field = symbols.split('("payloadNode")', 1)[1].split("DW_TAG_", 1)[0]
                        self.assertRegex(field, r'DW_AT_data_member_location\s+\(0x08\)')

    @unittest.skipUnless(WINDOWS, "Windows file sharing")
    def test_running_image_keeps_its_original_symbols(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary)
            source = package / "src" / "main.ly"
            source.parent.mkdir()
            source.write_text('extern fn getchar(): Int32;\nfn main(): Unit { val _ = getchar() }\n', encoding="utf-8")
            executable = package / "locked.exe"
            command = [COMPILER, "build", package, "-o", executable]
            built = subprocess.run(command, capture_output=True, timeout=40)
            self.assertEqual(built.returncode, 0, built.stderr)
            original = executable.read_bytes()
            symbols = self.symbols_for(executable)
            original_symbols = symbols.read_bytes()
            with subprocess.Popen([executable], stdin=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW) as running:
                try:
                    source.write_text('fn main(): Unit { print(43) }\n', encoding="utf-8")
                    replacement = subprocess.run(command, capture_output=True, timeout=40)
                    self.assertNotEqual(replacement.returncode, 0, replacement.stderr)
                    self.assertEqual(executable.read_bytes(), original)
                    self.assertEqual(symbols.read_bytes(), original_symbols)
                finally:
                    running.communicate(b"\n", timeout=10)
            replacement = subprocess.run(command, capture_output=True, timeout=40)
            self.assertEqual(replacement.returncode, 0, replacement.stderr)
            self.assertNotEqual(self.symbols_for(executable), symbols)
            self.assertEqual(symbols.read_bytes(), original_symbols)

    def test_async_step_symbols_keep_source_and_local_variables(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary)
            source = package / "src" / "main.ly"
            source.parent.mkdir()
            source.write_text(
                "async fn calculate(base: Int): Int {\n"
                "    val beforeWait = base + 1\n"
                "    await sleepMs(1)\n"
                "    val afterWait = beforeWait + 1\n"
                "    afterWait\n"
                "}\n"
                "async fn main(): Unit { print(await calculate(40)) }\n",
                encoding="utf-8",
            )
            built = subprocess.run([COMPILER, "build", package, "--debug-info"], capture_output=True, text=True, timeout=40)
            self.assertEqual(built.returncode, 0, built.stderr)
            executable = package / "build" / "debug" / ("app.exe" if WINDOWS else "app")
            symbols = self.inspect(executable)
            for fragment in ("lyra.fn.calculate.step", "lyra.fn.main.step", "beforeWait", "afterWait", "main.ly"):
                self.assertIn(fragment, symbols)
            executed = subprocess.run([executable], capture_output=True, text=True, timeout=10)
            self.assertEqual((executed.returncode, executed.stdout), (0, "42\n"))

    def test_optional_native_backtrace_keeps_the_logical_throw_location(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary)
            source = package / "src" / "main.ly"
            source.parent.mkdir()
            source.write_text(
                'fn leaf(): Unit { throw Exception("failed") }\n'
                'fn relay(): Unit { try { leaf() } catch { throw } }\n'
                'fn main(): Unit { relay() }\n', encoding="utf-8",
            )
            executable = package / ("trace failure.exe" if WINDOWS else "trace failure")
            built = subprocess.run([COMPILER, "build", package, "-o", executable], capture_output=True, text=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            for setting in ("0", "1"):
                with self.subTest(backtrace=setting):
                    result = subprocess.run([executable], capture_output=True, text=True, timeout=20,
                                            env={**os.environ, "LYRA_NATIVE_BACKTRACE": setting})
                    self.assertEqual((result.returncode, result.stdout), (1, ""))
                    self.assertEqual(result.stderr.count("throw at "), 1, result.stderr)
                    self.assertIn("main.ly:1:19", result.stderr)
                    self.assertEqual(result.stderr.count("call at "), 2, result.stderr)
                    if setting == "0":
                        self.assertNotIn("  native ", result.stderr)
                        continue
                    frame = re.search(r"(?m)^  native " + re.escape(executable.name) + r"\+(0x[0-9a-f]+)(.*)$", result.stderr)
                    self.assertIsNotNone(frame, result.stderr)
                    if WINDOWS:
                        self.assertIn("lyra.fn.leaf", frame[2], result.stderr)
                    elif symbolizer := (shutil.which("llvm-symbolizer-18") or shutil.which("llvm-symbolizer")):
                        symbol = subprocess.check_output([symbolizer, "--obj", executable, frame[1]], text=True, timeout=10)
                        self.assertIn("lyra.fn.leaf", symbol)
                        self.assertIn("main.ly:1:", symbol)


if __name__ == "__main__":
    unittest.main()
