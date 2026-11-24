import argparse
import contextlib
import functools
import hashlib
import http.client
import http.server
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import unittest

from platform_cli import assert_linkage, c_archive, executable, running


EXAMPLES = Path(__file__).resolve().parents[1] / 'examples'


class StaticRegistry(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append(self.path)
        super().do_GET()

    def log_message(self, *args):
        pass


class Installed(unittest.TestCase):
    def install(self, root):
        staged = root / 'staging'
        prefix = root / '搬迁工具链 relocated'
        if OPTIONS.sdk is not None:
            shutil.copytree(OPTIONS.sdk, staged)
        else:
            cmake, build = OPTIONS.build_tree
            result = subprocess.run([cmake, '--install', build, '--prefix', staged], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
        shutil.move(staged, prefix)
        return prefix

    def test_relocated_distribution_builds_from_a_locked_source_release(self):
        with tempfile.TemporaryDirectory(prefix='lyra-relocation-') as temporary:
            root = Path(temporary)
            prefix = self.install(root)
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
                'src/.macros/declare.ly': 'fn declare(target: String, argument: String): Syntax { syntaxCode("fn title(): String { " + macroQuote(macroInput("title")) + " }") }',
                'src/.expressions/stamp.ly': 'fn stamp(target: String, argument: String): Syntax { syntaxString(macroInput("snapshot")) }',
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

    def test_relocated_sdk_runs_both_application_workspaces(self):
        with tempfile.TemporaryDirectory(prefix='lyra-apps-') as temporary:
            root = Path(temporary)
            compiler = executable(self.install(root) / 'bin/lyra')
            environment = {**os.environ, 'LYRA_CACHE': str(root / '编译 cache')}
            isolated = {**environment, 'PATH': '', 'LIB': '', 'INCLUDE': ''}
            jsonlines = root / 'JSONL 工作区'
            service = root / 'HTTP 工作区'
            for name, destination in (('jsonlines', jsonlines), ('http-sqlite', service)):
                shutil.copytree(EXAMPLES / name, destination, ignore=shutil.ignore_patterns('build', '.lyra', '__pycache__'))

            def run(*args):
                result = subprocess.run([compiler, *args, '--locked', '--offline'], cwd=root, env=environment,
                                        capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stderr)
                return result.stdout

            self.assertIn('ok tools/metrics/readings.ly', run('test', jsonlines))
            run('check', service / 'users')
            database = root / '查询 数据库.db'
            with contextlib.closing(sqlite3.connect(database)) as connection, connection:
                connection.executescript("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT); INSERT INTO users VALUES(7,'Ada');")
            profiles = (('debug', ()), ('release', ('--release',)))
            verified = []
            for profile, flags in profiles:
                with self.subTest(profile=profile):
                    cli = executable(root / 'deploy' / profile / 'stats')
                    server = executable(root / 'deploy' / profile / 'users')
                    run('build', jsonlines / 'stats', *flags, '-o', cli)
                    run('build', service, *flags, '-o', server)
                    result = subprocess.run([cli], input=b'{"station":"west","delta":7}\ninvalid\n',
                                            cwd=root, env=isolated, capture_output=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(json.loads(result.stdout), {'rows': 1, 'total': 7, 'rejected': 1})
                    self.assertIn(b'line 2:', result.stderr)
                    with running([server, database], cwd=root, env=isolated) as (process, output):
                        port = int(output.line())
                        for user, expected in ((7, 200), (8, 404)):
                            with contextlib.closing(http.client.HTTPConnection('127.0.0.1', port, timeout=5)) as connection:
                                connection.request('GET', f'/users/{user}')
                                response = connection.getresponse()
                                self.assertEqual(response.status, expected)
                                body = response.read()
                                if expected == 200:
                                    self.assertEqual(json.loads(body), {'id': 7, 'name': 'Ada'})
                        self.assertIsNone(process.poll())
                    assert_linkage(self, cli)
                    assert_linkage(self, server, 'libsqlite3.so.0')
                    verified.append(profile)
            if OPTIONS.deployment_bundle is not None and len(verified) == len(profiles):
                bundle = OPTIONS.deployment_bundle
                bundle.mkdir(parents=True)
                shutil.copy2(database, bundle / 'users.db')
                shutil.copy2(Path(__file__).with_name('windows-deploy-smoke.ps1'), bundle / 'verify.ps1')
                for profile in verified:
                    destination = bundle / profile
                    destination.mkdir()
                    for application in ('stats', 'users'):
                        name = executable(Path(application))
                        shutil.copy2(root / 'deploy' / profile / name, destination / name)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Verify a relocated Lyra SDK and standalone applications.')
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--sdk', type=Path, help='path to an unpacked SDK prefix')
    source.add_argument('--build-tree', nargs=2, type=Path, metavar=('CMAKE', 'BUILD'))
    parser.add_argument('--deployment-bundle', type=Path, help='write the Windows standalone apps and smoke script into a new directory')
    OPTIONS, tests = parser.parse_known_args()
    if OPTIONS.sdk is not None:
        OPTIONS.sdk = OPTIONS.sdk.resolve()
        if not executable(OPTIONS.sdk / 'bin/lyra').is_file():
            parser.error('expected an SDK prefix containing bin/lyra')
    if OPTIONS.deployment_bundle is not None:
        if os.name != 'nt' or OPTIONS.sdk is None:
            parser.error('--deployment-bundle requires --sdk on Windows')
        OPTIONS.deployment_bundle = OPTIONS.deployment_bundle.resolve()
        if OPTIONS.deployment_bundle.exists():
            parser.error('--deployment-bundle must name a new directory')
    unittest.main(argv=[sys.argv[0], *tests])
