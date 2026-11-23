import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()
EXAMPLE = Path(__file__).resolve().parents[1] / "examples/jsonlines"


class ExternalJsonLines(unittest.TestCase):
    def test_publish_then_build_from_verified_offline_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            publisher = root / "publisher" / "metrics"
            consumer = root / "consumer" / "stats"
            registry = root / "private-center"
            env = {**os.environ, "LYRA_CACHE": str(root / "user-cache")}

            def cli(*args):
                done = subprocess.run([COMPILER, *args], cwd=root, env=env, capture_output=True, timeout=40)
                self.assertEqual(done.returncode, 0, done.stderr.decode())
                return done

            for name, destination in (("metrics", publisher), ("stats", consumer)):
                shutil.copytree(EXAMPLE / name, destination, ignore=shutil.ignore_patterns("build", ".lyra", "lyra.lock"))
            manifest = consumer / "lyra.json5"
            settings = json.loads(manifest.read_text())
            dependency = settings["dependencies"]["metrics"]
            del dependency["path"]
            dependency["registry"] = str(registry)
            manifest.write_text(json.dumps(settings))
            cli("check", publisher)
            cli("test", publisher)
            cli("pack", publisher, "--registry", registry)
            cli("lock", consumer)
            cli("fetch", consumer, "--locked")
            lock = (consumer / "lyra.lock").read_bytes()
            publisher.rename(root / "unmounted-publisher")
            registry.rename(root / "unmounted-center")
            cli("fetch", consumer, "--locked", "--offline")

            data = (b'{"station":"west","delta":9007199254740993}\n'
                    + '{"station":"东\\"区","delta":-2}\r\n'.encode()
                    + b'{"delta":"bad","station":"east"}\ninvalid json\n\xff\n'
                    + b'{"station":"tail","delta":1}')
            for profile, flags in (("debug", []), ("release", ["--release"])):
                with self.subTest(profile=profile):
                    app = root / profile
                    cli("build", consumer, "--locked", "--offline", "-o", app, *flags)
                    for text, expected in ((data, {"rows": 3, "total": 9007199254740992, "rejected": 3}),
                                           (b"", {"rows": 0, "total": 0, "rejected": 0})):
                        done = subprocess.run([app], input=text, cwd=root, capture_output=True, timeout=5)
                        self.assertEqual(done.returncode, 0, done.stderr.decode())
                        self.assertEqual(json.loads(done.stdout), expected)
                        errors = done.stderr.decode().splitlines()
                        self.assertEqual([line.split(":", 1)[0] for line in errors], ["line 3", "line 4", "line 5"] if text else [])
                    bad_input = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
                    try:
                        done = subprocess.run([app], stdin=bad_input, cwd=root, capture_output=True, timeout=5)
                    finally:
                        os.close(bad_input)
                    self.assertNotEqual(done.returncode, 0)
                    self.assertEqual(done.stdout, b"")
                    self.assertIn(b"cannot read stdin", done.stderr)
                    dynamic = subprocess.check_output(["readelf", "-dW", app], text=True)
                    needed = [line.split("[")[1].split("]")[0] for line in dynamic.splitlines() if "(NEEDED)" in line]
                    self.assertEqual(needed, ["libc.so.6"])
            self.assertEqual((consumer / "lyra.lock").read_bytes(), lock)


if __name__ == "__main__":
    unittest.main()
