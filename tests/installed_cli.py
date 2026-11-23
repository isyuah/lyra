import functools
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest


CMAKE = Path(sys.argv.pop(1)).resolve()
BUILD = Path(sys.argv.pop(1)).resolve()


class StaticRegistry(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append(self.path)
        super().do_GET()

    def log_message(self, *args):
        pass


class Installed(unittest.TestCase):
    def test_relocated_distribution_builds_from_a_locked_source_release(self):
        with tempfile.TemporaryDirectory(prefix='lyra-relocation-') as temporary:
            root = Path(temporary)
            staged = root / 'staging'
            prefix = root / 'relocated toolchain'
            cache = root / 'private cache'
            environment = {**os.environ, 'LYRA_CACHE': str(cache), 'NO_PROXY': '127.0.0.1', 'no_proxy': '127.0.0.1'}

            def run(*args, good=True):
                process = subprocess.run(args, cwd=root, env=environment, capture_output=True, text=True, timeout=60)
                self.assertEqual(process.returncode == 0, good, f'{args}\n{process.stdout}{process.stderr}')
                return process

            def write(path, text):
                path.parent.mkdir(exist_ok=True, parents=True)
                path.write_text(text, encoding='utf-8')

            def manifest(package, **changes):
                path = package / 'lyra.json5'
                write(path, json.dumps({**json.loads(path.read_text()), **changes}))

            run(CMAKE, '--install', BUILD, '--prefix', staged)
            shutil.copytree(staged, prefix)
            shutil.rmtree(staged)
            compiler = prefix / 'bin/lyra'
            write(prefix / 'share/lyra/std/installprobe/src/identity.ly', 'pub fn identity(): String { "relocated" }')
            app = root / 'consumer app'
            library = root / 'source producer'
            run(compiler, 'new', app, '--name', 'release/consumer')
            run(compiler, 'new', library, '--name', 'release/catalog', '--lib')
            self.assertEqual(run(compiler, 'run', app).stdout.splitlines(), ['42'])

            inputs = {'declare': {'title': 'inputs/title.txt'}, 'stamp': {'snapshot': 'inputs/snapshot.txt'}}
            manifest(library, macroInputs=inputs)
            files = {
                'src/.macros/declare.ly': 'fn declare(target: String, argument: String): String { "fn title(): String { " + macroQuote(macroInput("title")) + " }" }',
                'src/.expressions/stamp.ly': 'fn stamp(target: String, argument: String): String { macroQuote(macroInput("snapshot")) }',
                'inputs/title.txt': '打包时的标题',
                'inputs/snapshot.txt': 'v1',
            }
            for relative, content in files.items():
                write(library / relative, content)
            write(library / 'inputs/unlisted.txt', 'not for distribution')
            write(library / 'src/lib.ly', '#declare\n@trace\npub fn banner(): String { title() }')
            registry = root / 'registry'
            run(compiler, 'pack', library, '--registry', registry)

            handler = functools.partial(StaticRegistry, directory=registry)
            with http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler) as server:
                server.requests = []
                thread = threading.Thread(target=server.serve_forever)
                thread.start()
                try:
                    origin = f'http://127.0.0.1:{server.server_port}'
                    manifest(app, dependencies={'catalog': {'name': 'release/catalog', 'version': '^0.1.0', 'registry': origin}})
                    run(compiler, 'lock', app)
                finally:
                    server.shutdown()
                    thread.join()

            lock = app / 'lyra.lock'
            frozen = lock.read_bytes()
            package = next(item for item in json.loads(frozen)['packages'] if item['name'] == 'release/catalog')
            digest = package['sha256']
            self.assertEqual((package['version'], package['source']), ('0.1.0', origin))
            self.assertEqual(server.requests, ['/index/release/catalog.json', f'/artifacts/{digest}.lypkg'])
            self.assertEqual(hashlib.sha256((cache / 'artifacts' / f'{digest}.lypkg').read_bytes()).hexdigest(), digest)
            source = cache / 'sources' / digest
            for relative, content in files.items():
                self.assertEqual((source / relative).read_text(encoding='utf-8'), content)
            self.assertFalse((source / 'inputs/unlisted.txt').exists())
            shutil.rmtree(library)
            shutil.rmtree(registry)

            native = root / 'native fixture'
            write(native / 'scale.c', '#include <stdint.h>\nint64_t distributionScale(int64_t number) { return number * 7; }\n')
            obj = native / 'scale.o'
            archive = native / 'lib scale.a'
            run('clang-18', '-c', native / 'scale.c', '-o', obj)
            run('ar', 'rcs', archive, obj)
            write(app / 'src/main.ly', '''import catalog.banner
import std.installprobe.identity
extern fn distributionScale(number: Int): Int;
pub fn label(): String { identity() + ": " + banner() }
pub fn score(): Int { distributionScale(6) }
fn main(): Unit { print(label()); print(score()) }
''')
            write(app / 'tests/contract.ly', '''import self.label
import self.score
import std.io.assert
fn main(): Unit { assert(label() == "relocated: 打包时的标题", "wrong installed input"); assert(score() == 42, "wrong C ABI result") }
''')
            blocked = root / 'offline guard' / 'curl'
            write(blocked, '#!/bin/sh\n: > "$LYRA_NETWORK_CALLED"\nexit 97\n')
            blocked.chmod(0o755)
            attempted = root / 'network was requested'
            environment.update(PATH=str(blocked.parent) + os.pathsep + environment['PATH'], LYRA_NETWORK_CALLED=str(attempted))
            frozen_args = ('--offline', '--locked')
            run(compiler, 'fetch', app, *frozen_args)
            for mode in ([], ['--release']):
                args = (*frozen_args, *mode, '--link', archive)
                self.assertIn('ok release/consumer/contract.ly', run(compiler, 'test', app, *args).stdout)
                result = run(compiler, 'run', app, *args)
                self.assertEqual((result.stdout.splitlines(), result.stderr), (['trace: banner', 'relocated: 打包时的标题', '42'], ''))
            self.assertEqual(lock.read_bytes(), frozen)
            shutil.rmtree(cache)
            error = run(compiler, 'fetch', app, *frozen_args, good=False)
            self.assertIn(digest, error.stderr)
            self.assertEqual(lock.read_bytes(), frozen)
            self.assertFalse(attempted.exists(), 'an offline command invoked the network transport')


if __name__ == '__main__':
    unittest.main()
