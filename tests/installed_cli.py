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

from platform_cli import assert_linkage, c_archive, executable


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
            prefix = root / '搬迁工具链 relocated'
            cache = root / '独立缓存 private'
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
            compiler = executable(prefix / 'bin/lyra')
            write(prefix / 'share/lyra/std/installprobe/src/identity.ly', 'pub fn identity(): String { "relocated" }')
            app = root / '使用方 consumer app'
            library = root / 'source producer'
            run(compiler, 'new', app, '--name', 'release/consumer')
            run(compiler, 'new', library, '--name', 'release/catalog', '--lib')
            frozen_args = ('--offline', '--locked')
            for package, name in ((app, 'consumer'), (library, 'catalog')):
                run(compiler, 'lock', package, '--offline')
                self.assertEqual(run(compiler, 'test', package, *frozen_args).stdout.splitlines(), [f'ok release/{name}/answer.ly'])
            self.assertEqual(run(compiler, 'run', app, *frozen_args).stdout.splitlines(), ['42'])

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
            write(library / 'src/lib.ly', '#declare\n@trace\npub fn banner(): String { title() + ":" + #stamp() }')
            registry = root / 'registry'
            run(compiler, 'pack', library, '--registry', registry)

            handler = functools.partial(StaticRegistry, directory=registry)
            server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
            server.requests = []
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            self.addCleanup(server.server_close)
            self.addCleanup(thread.join)
            self.addCleanup(server.shutdown)
            origin = f'http://127.0.0.1:{server.server_port}'
            manifest(app, dependencies={'catalog': {'name': 'release/catalog', 'version': '^0.1.0', 'registry': origin}})
            run(compiler, 'lock', app)

            lock = app / 'lyra.lock'
            frozen = lock.read_bytes()
            package = next(item for item in json.loads(frozen)['packages'] if item['name'] == 'release/catalog')
            digest = package['sha256']
            self.assertEqual((package['version'], package['source']), ('0.1.0', origin))
            online_requests = ['/index/release/catalog.json', f'/artifacts/{digest}.lypkg']
            self.assertEqual(server.requests, online_requests)
            self.assertEqual(hashlib.sha256((cache / 'artifacts' / f'{digest}.lypkg').read_bytes()).hexdigest(), digest)
            source = cache / 'sources' / digest
            for relative, content in files.items():
                self.assertEqual((source / relative).read_text(encoding='utf-8'), content)
            self.assertFalse((source / 'inputs/unlisted.txt').exists())
            shutil.rmtree(library)
            shutil.rmtree(registry)

            native = root / 'native fixture'
            write(native / 'read.c', '''#define _XOPEN_SOURCE 700
#include <lyra/string.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define _WIN32_WINNT 0x0A00
#include <windows.h>
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static SYNCHRONIZATION_BARRIER gate;
static BOOL CALLBACK initGate(PINIT_ONCE state, PVOID param, PVOID *context) {
    (void)state;
    (void)param;
    (void)context;
    return InitializeSynchronizationBarrier(&gate, 2, -1);
}
void distributionMeet(void) {
    if (!InitOnceExecuteOnce(&once, initGate, NULL, NULL)) abort();
    EnterSynchronizationBarrier(&gate, SYNCHRONIZATION_BARRIER_FLAGS_NO_DELETE);
}
#else
#include <pthread.h>
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_barrier_t gate;
static void initGate(void) { if (pthread_barrier_init(&gate, NULL, 2)) abort(); }
void distributionMeet(void) {
    if (pthread_once(&once, initGate)) abort();
    int status = pthread_barrier_wait(&gate);
    if (status != 0 && status != PTHREAD_BARRIER_SERIAL_THREAD) abort();
}
#endif

int64_t distributionRead(const LyraString *value, const LyraString *empty, int64_t number) {
    static const unsigned char expected[] = {0xe4, 0xb8, 0xad, 0, 'x', 0};
    LyraStringView text = lyraStringView(value);
    LyraStringView blank = lyraStringView(empty);
    LyraStringView absent = lyraStringView(NULL);
    int emptyViewsMatch = blank.data && blank.size == 0 && !absent.data && absent.size == 0;
    volatile uintptr_t maskedAddress = ~(uintptr_t)text.data;
    distributionMeet();
    distributionMeet();
    const char *data = (const char *)~maskedAddress;
    if (!emptyViewsMatch || !data || text.size != sizeof(expected) || memcmp(data, expected, text.size)) return -1;
    return number * 7;
}
''')
            archive = c_archive(native / 'read.c', native / 'lib fixture', '-O2', '-Wall', '-Wextra', '-Werror', '-I', prefix / 'include')
            write(app / 'src/main.ly', '''import catalog.banner
import std.installprobe.identity
extern fn distributionRead(value: String, empty: String, number: Int): Int;
extern fn distributionMeet(): Unit;
pub fn label(): String { identity() + ": " + banner() }
pub async fn score(): Int {
    val reading = spawn blocking { distributionRead("中" + "\\0x\\0", "", 6) }
    await blocking { distributionMeet() }
    gcCollect()
    distributionMeet()
    await reading
}
async fn main(): Unit { print(label()); print(await score()) }
''')
            (app / 'tests/answer.ly').unlink()
            write(app / 'tests/contract.ly', '''import self.label
import self.score
import std.io.assert
async fn main(): Unit { assert(label() == "relocated: 打包时的标题:v1", "wrong installed input"); assert(await score() == 42, "wrong C ABI result") }
''')
            environment['UV_THREADPOOL_SIZE'] = '2'
            run(compiler, 'fetch', app, *frozen_args)
            for mode in ([], ['--release']):
                args = (*frozen_args, *mode, '--link', archive)
                self.assertIn('ok release/consumer/contract.ly', run(compiler, 'test', app, *args).stdout)
                result = run(compiler, 'run', app, *args)
                self.assertEqual((result.stdout.splitlines(), result.stderr), (['trace: banner', 'relocated: 打包时的标题:v1', '42'], ''))
                program = executable(app / 'build' / ('release' if mode else 'debug') / 'app')
                assert_linkage(self, program)
                standalone = executable(root / '独立 standalone')
                shutil.copy2(program, standalone)
                isolated = subprocess.run([standalone], cwd=root, capture_output=True, text=True, timeout=10,
                                          env={**environment, 'PATH': '', 'LIB': '', 'INCLUDE': ''})
                self.assertEqual((isolated.returncode, isolated.stdout.splitlines(), isolated.stderr),
                                 (0, ['trace: banner', 'relocated: 打包时的标题:v1', '42'], ''))
            self.assertEqual(lock.read_bytes(), frozen)
            shutil.rmtree(cache)
            error = run(compiler, 'fetch', app, *frozen_args, good=False)
            self.assertIn(digest, error.stderr)
            self.assertEqual(lock.read_bytes(), frozen)
            self.assertEqual(server.requests, online_requests, 'an offline command invoked the network transport')


if __name__ == '__main__':
    unittest.main()
