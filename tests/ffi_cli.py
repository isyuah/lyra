import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from platform_cli import WINDOWS, c_archive, executable


COMPILER = Path(sys.argv.pop(1)).resolve()
CMAKE = sys.argv.pop(1)

CALLBACK_C = '''#define _WIN32_WINNT 0x0A00
#define _XOPEN_SOURCE 700
#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
static SYNCHRONIZATION_BARRIER gate;
static HANDLE thread;
void ffiMeet(void) { EnterSynchronizationBarrier(&gate, SYNCHRONIZATION_BARRIER_FLAGS_NO_DELETE); }
#else
#include <pthread.h>
static pthread_barrier_t gate;
static pthread_t thread;
void ffiMeet(void) {
    int status = pthread_barrier_wait(&gate);
    if (status != 0 && status != PTHREAD_BARRIER_SERIAL_THREAD) abort();
}
#endif

typedef int64_t (*Callback)(void *, int16_t, double, uint8_t);
typedef void (*Notify)(void *);
static Callback saved;
static uintptr_t hiddenContext;
static int64_t threadResult;

int32_t ffiRead(const int32_t *values) { return values[0] + values[1]; }
void ffiSet(void *function, void *context) {
    saved = (Callback)function;
    hiddenContext = ~(uintptr_t)context;
}
int64_t ffiRun(int16_t amount, double scale, uint8_t enabled) {
    return saved((void *)~hiddenContext, amount, scale, enabled);
}
void ffiNotify(void *function, void *context) { ((Notify)function)(context); }

static void runCallbacks(void) {
    threadResult = ffiRun(7, 0.5, 1);
    if (ffiRun(0, 0.0, 0) != -9) abort();
}

#ifdef _WIN32
static DWORD WINAPI worker(void *unused) {
    (void)unused;
    runCallbacks();
    return 0;
}
void ffiStart(void) {
    if (!InitializeSynchronizationBarrier(&gate, 2, -1)) abort();
    thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!thread) abort();
    ffiMeet();
}
int64_t ffiFinish(void) {
    ffiMeet();
    if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) abort();
    CloseHandle(thread);
    DeleteSynchronizationBarrier(&gate);
    return threadResult;
}
#else
static void *worker(void *unused) {
    (void)unused;
    runCallbacks();
    return NULL;
}
void ffiStart(void) {
    if (pthread_barrier_init(&gate, NULL, 2) || pthread_create(&thread, NULL, worker, NULL)) abort();
    ffiMeet();
}
int64_t ffiFinish(void) {
    ffiMeet();
    if (pthread_join(thread, NULL) || pthread_barrier_destroy(&gate)) abort();
    return threadResult;
}
#endif
'''


class Ffi(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='lyra-ffi-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.src = self.root / 'src'
        self.src.mkdir()
        self.main = self.src / 'main.ly'

    def command(self, *args, compiler=COMPILER, environment=None, success=True):
        result = subprocess.run([compiler, *args], cwd=self.root, env=environment, capture_output=True, text=True,
                                encoding='utf-8', timeout=45)
        self.assertEqual(result.returncode == 0, success, f'{args}\n{result.stdout}{result.stderr}')
        return result

    def archive(self, text, *flags):
        source = self.root / 'fixture.c'
        source.write_text(text, encoding='utf-8')
        return c_archive(source, self.root / 'ffiFixture', '-O2', '-Wall', '-Wextra', '-Werror', *flags)

    def build_and_run(self, source, archive, expected):
        self.main.write_text(source, encoding='utf-8')
        for mode in ([], ['--release']):
            with self.subTest(mode=mode):
                app = executable(self.root / 'app')
                self.command('build', self.root, *mode, '--link', archive, '-o', app)
                result = subprocess.run([app], capture_output=True, text=True, encoding='utf-8', timeout=25)
                self.assertEqual((result.returncode, result.stdout.splitlines(), result.stderr), (0, expected, ''))

    def test_aligned_native_pointer_loads_and_stores(self):
        archive = self.archive(CALLBACK_C)
        self.build_and_run('''extern fn ffiRead(pointer: CPtr): Int32;
fn markedOffset(): Int { print("offset"); 4 }
fn markedValue(): Int32 { print("value"); 21 }
fn main(): Unit {
    print(CPtr.nullPtr().isNull())
    print(try { unsafe { val invalid = CPtr.alloc(-1); false } } catch { true })
    val memory = unsafe { CPtr.alloc(bytes: 32) }
    defer unsafe { memory.free() }
    print(memory.isNull())
    unsafe {
        memory.writeInt32(0, 19)
        memory.writeInt32(value: markedValue(), offset: markedOffset())
        print(ffiRead(memory))
        memory.writeCPtr(8, memory.offset(4))
        print(memory.readCPtr(8).readInt32(0))
        memory.offset(16).writeFloat64(0, 1.5)
        print(memory.readFloat64(16))
        memory.writeUInt64(24, 18446744073709551615)
        print(memory.readUInt64(24))
    }
}''', archive, ['true', 'true', 'false', 'value', 'offset', '40', '21', '1.5', '18446744073709551615'])

    def test_callbacks_survive_gc_and_contain_exceptions_on_native_threads(self):
        archive = self.archive(CALLBACK_C)
        self.build_and_run('''extern fn ffiSet(function: CPtr, context: CPtr): Unit;
extern fn ffiRun(amount: Int16, scale: Float64, enabled: Bool): Int;
extern fn ffiNotify(function: CPtr, context: CPtr): Unit;
extern fn ffiStart(): Unit;
extern fn ffiMeet(): Unit;
extern fn ffiFinish(): Int;
class Captured(val name: String, val numbers: List<Int>)
exception CallbackProblem
fn make(): CCallback {
    val captured = Captured("saved " + "capture", [30, 11])
    unsafe { CCallback.register((amount: Int16, scale: Float64, enabled: Bool): Int => {
        if !enabled { throw CallbackProblem(captured.name + ":" + amount.toString()) }
        if amount == 7 { ffiMeet(); ffiMeet() }
        gcCollect()
        print(captured.name)
        captured.numbers[0] + amount + (if scale == 0.5 { 5 } else { 0 })
    }, fallback: -9) }
}
fn installWithoutLocalRoot(): Unit {
    val handle = make()
    unsafe { ffiSet(handle.function(), handle.context()) }
}
fn main(): Unit {
    installWithoutLocalRoot()
    gcCollect()
    print(ffiRun(1, 0.0, true))
    val callback = make()
    unsafe { ffiSet(callback.function(), callback.context()) }
    ffiStart()
    gcCollect()
    print(ffiFinish())
    gcCollect()
    print(ffiRun(1, 0.0, false))
    print(match callback.takeError() { Some(error) => error.message, None => "missing" })
    print(callback.takeError() == None)
    val notify = unsafe { CCallback.register((): Unit => { throw Exception("notification") }) }
    unsafe { ffiNotify(notify.function(), notify.context()); notify.close() }
    print(match notify.takeError() { Some(error) => error.message, None => "missing" })
    unsafe {
        ffiSet(CPtr.nullPtr(), CPtr.nullPtr())
        callback.close(); callback.close()
        print(callback.function().isNull()); print(callback.context().isNull())
    }
}''',
            archive, ['saved capture', '31', 'saved capture', '42', '-9', 'saved capture:0', 'true', 'notification', 'true', 'true'])

    def test_unsafe_boundary_and_callback_signature_errors(self):
        cases = [
            ('CPtr.alloc(4)', 'alloc'),
            ('CPtr.nullPtr().readInt32(0)', 'readInt32'),
            ('CPtr.nullPtr().free()', 'free'),
            ('CCallback.register((): Unit => {})', 'register'),
            ('unsafe { CCallback.register((): Int => { 1 }) }', 'register'),
            ('unsafe { CCallback.register((text: String): Unit => { print(text) }) }', '(text:'),
            ('unsafe { CCallback.register(async (): Int => { 1 }, fallback: 0) }', 'async'),
        ]
        for expression, marker in cases:
            with self.subTest(expression=expression):
                source = 'fn main(): Unit { val probe = ' + expression + ' }'
                self.main.write_text(source, encoding='utf-8')
                result = self.command('check', self.root, '--diagnostics=json', success=False)
                diagnostic, = json.loads(result.stderr)
                self.assertEqual(diagnostic['code'], 'LYF0002', result.stderr)
                self.assertEqual(diagnostic['range']['start']['offset'], source.index(marker))

    def test_relocated_sdk_imports_and_links_header_without_host_tools(self):
        sdk = self.root / 'SDK 搬迁'
        staged = self.root / 'staged'
        installed = subprocess.run([CMAKE, '--install', COMPILER.parent.parent, '--prefix', staged], capture_output=True, text=True, encoding='utf-8', timeout=120)
        self.assertEqual(installed.returncode, 0, installed.stderr)
        shutil.move(staged, sdk)
        compiler = executable(sdk / 'bin/lyra')
        headers = self.root / '头 文件'
        headers.mkdir()
        (headers / 'shared.h').write_text('#include <stdint.h>\ntypedef int16_t Small;\nint hidden(void);\n')
        header = self.root / '接口 API.h'
        header.write_text('''#include <shared.h>
#include <stddef.h>
#if EXPOSE_API
long platformWidth(long val);
Small match(Small value);
_Bool invert(_Bool value);
size_t measure(const int32_t *values, size_t count);
Small match(Small renamed);
#endif
''', encoding='utf-8')
        archive = self.archive('''#include <stdint.h>
#include <stddef.h>
long platformWidth(long val) { return val + sizeof(long); }
int16_t match(int16_t value) { return value - 1; }
_Bool invert(_Bool value) { return !value; }
size_t measure(const int32_t *values, size_t count) { return values ? count : 0; }
''')
        environment = {'PATH': ''}
        if WINDOWS:
            environment['SystemRoot'] = os.environ['SystemRoot']
        binding = self.src / 'binding.ly'
        result = self.command('import-c', header, '-I', headers, '-DEXPOSE_API=1', '-o', binding, '--diagnostics=json',
                              compiler=compiler, environment=environment)
        self.assertEqual(json.loads(result.stderr), [])
        self.assertEqual(result.stdout, '')
        code = binding.read_text(encoding='utf-8')
        native_long = 'Int32' if WINDOWS else 'Int64'
        self.assertIn(f'`platformWidth`(`val`: {native_long}): {native_long}', code)
        self.assertEqual(code.count('`match`('), 1)
        self.assertNotIn('hidden', code)
        self.command('import-c', header, '-I', headers, '-DEXPOSE_API=1', '-o', binding, compiler=compiler,
                     environment=environment, success=False)
        self.assertEqual(binding.read_text(encoding='utf-8'), code)
        self.main.write_text('''fn main(): Unit {
    print(platformWidth(1))
    print(`match`(-2))
    print(invert(false)); print(invert(true))
    unsafe {
        val memory = CPtr.alloc(4)
        defer memory.free()
        print(measure(memory, 7))
    }
}''', encoding='utf-8')
        ran = self.command('run', self.root, '--release', '--link', archive, compiler=compiler)
        self.assertEqual(ran.stdout.splitlines(), ['5' if WINDOWS else '9', '-3', 'true', 'false', '7'])

        registry = self.root / 'registry'
        def package(directory, name, code, **manifest):
            root = self.root / directory
            (root / 'src').mkdir(parents=True)
            (root / 'src/main.ly').write_text(code, encoding='utf-8')
            (root / 'lyra.json5').write_text(json.dumps(dict(name=name, version='1.0.0', **manifest)), encoding='utf-8')
            return root

        leaf = package('C 原生层', 'native/leaf', 'pub fn marker(): Int { 1 }', kind='lib', native={
            'sources': ['native/count.c'], 'headers': ['native/include/helper.h'], 'includeDirs': ['native/include'],
        })
        (leaf / 'native/include').mkdir(parents=True)
        (leaf / 'native/include/helper.h').write_text('#define ADDEND 5\n', encoding='utf-8')
        (leaf / 'native/count.c').write_text('''#include <helper.h>
#include <stdlib.h>
#include <stdint.h>
int32_t nativeCount(int32_t amount) { return abs(amount) + ADDEND; }
''', encoding='utf-8')
        first, second = self.root / 'first.lypkg', self.root / 'second.lypkg'
        self.command('pack', leaf, '-o', first, compiler=compiler, environment=environment)
        (leaf / 'native/unused.c').write_text('#error unused native input\n')
        self.command('pack', leaf, '-o', second, compiler=compiler, environment=environment)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.command('pack', leaf, '--registry', registry, compiler=compiler, environment=environment)
        bridge = package('bridge', 'native/bridge', 'extern fn nativeCount(value: Int32): Int32;\n'
                         'pub fn count(value: Int32): Int32 { nativeCount(value) }', kind='lib', dependencies={
                             'leaf': {'name': 'native/leaf', 'version': '1.0.0', 'registry': 'self'},
                         })
        self.command('pack', bridge, '--registry', registry, compiler=compiler, environment=environment)
        app = package('测试 应用', 'native/app', 'import bridge.{count}\nfn main(): Unit { print(count(-7)) }', dependencies={
            'bridge': {'name': 'native/bridge', 'version': '1.0.0', 'registry': str(registry)},
        })
        ran = self.command('run', app, '--offline', '--release', compiler=compiler, environment=environment)
        self.assertEqual(ran.stdout.splitlines(), ['12'])

    def test_native_archive_requires_a_valid_archive_for_this_target(self):
        current = 'windows-x64' if WINDOWS else 'linux-x64'
        other = 'linux-x64' if WINDOWS else 'windows-x64'
        native = self.root / 'native'
        native.mkdir()
        archive = self.archive('int fixture(void) { return 31; }')
        shutil.copyfile(archive, native / 'fixture.a')
        (native / 'invalid.a').write_bytes(b'not an archive')
        self.main.write_text('extern fn fixture(): Int32;\nfn main(): Unit { print(fixture()) }', encoding='utf-8')
        manifest = self.root / 'lyra.json5'

        def configure(archives):
            manifest.write_text(json.dumps(dict(name='fixture', version='1.0.0', native={'archives': archives})))

        configure({other: ['native/invalid.a'], current: ['native/fixture.a']})
        self.assertEqual(self.command('run', self.root).stdout.splitlines(), ['31'])
        configure({other: ['native/fixture.a']})
        missing = self.command('build', self.root, '--diagnostics=json', success=False)
        self.assertEqual(json.loads(missing.stderr)[0]['code'], 'LYB0004')
        configure({current: ['native/invalid.a']})
        invalid = self.command('build', self.root, '--diagnostics=json', success=False)
        self.assertEqual(json.loads(invalid.stderr)[0]['code'], 'LYB0004')

    def test_import_keeps_supported_bindings_and_diagnoses_unmapped_declarations(self):
        header = self.root / 'case.h'
        header.write_text('''struct Data { int value; };
extern int counter;
struct Data byValue(void);
int variableArgs(int amount, ...);
static inline int inlineHelper(void) { return 0; }
int supported(struct Data *opaque);
''', encoding='utf-8')
        imported = self.command('import-c', header, '--diagnostics=json')
        self.assertEqual(imported.stdout.splitlines(), ['pub extern fn `supported`(`opaque`: CPtr): Int32;'])
        diagnostics = json.loads(imported.stderr)
        self.assertEqual([(issue['code'], issue['severity'], issue['range']['start']['line']) for issue in diagnostics],
                         [('LYF0004', 'warning', line) for line in (2, 3, 4, 5)])
        self.assertTrue(all(Path(issue['file']) == header for issue in diagnostics))

        header.write_text('#include "definitelyAbsent.h"\nint supported(void);\n', encoding='utf-8')
        imported = self.command('import-c', header, '--diagnostics=json', success=False)
        diagnostic, = json.loads(imported.stderr)
        self.assertEqual(imported.stdout, '')
        self.assertEqual((diagnostic['code'], diagnostic['range']['start']['line']), ('LYF0003', 1))

    def test_import_adapter_uses_c_abi_for_records_inline_globals_and_variadic_promotions(self):
        native = self.root / 'native'
        native.mkdir()
        header = native / 'header.h'
        header.write_text('''#include <stdint.h>
typedef struct { int32_t x; double y; } Point;
struct Mix { Point *pointer; int16_t offset; };
extern int32_t counter;
extern const int32_t base;
Point addPoint(Point first, Point second);
static inline int32_t builtIn(int32_t value) { return value + EXTRA; }
double sum(int32_t count, ...);
''', encoding='utf-8')
        (native / 'fixture.c').write_text('''#define EXTRA 7
#include "header.h"
#include <stdarg.h>
int32_t counter = 5;
const int32_t base = 11;
Point addPoint(Point first, Point second) { return (Point){ first.x + second.x, first.y + second.y }; }
double sum(int32_t count, ...) {
    va_list args;
    va_start(args, count);
    int integer = va_arg(args, int);
    double real = va_arg(args, double);
    va_end(args);
    return count + integer + real;
}
''', encoding='utf-8')
        bindings, adapter = self.src / 'binding.ly', native / 'bridge.c'
        inputs = ('import-c', header, '-DEXTRA=7', '--adapter', adapter, '--diagnostics=json')
        request = self.command(*inputs, '-o', bindings, '--varargs', 'sum=sumValues(Int16,Float32)')
        self.assertEqual(json.loads(request.stderr), [])
        self.command(*inputs, '-o', self.src / 'second.ly', success=False)
        self.assertFalse((self.src / 'second.ly').exists())
        (self.root / 'lyra.json5').write_text(json.dumps(dict(name='bridge', version='1.0.0', native={
            'sources': ['native/bridge.c', 'native/fixture.c'], 'headers': ['native/header.h'],
        })))
        self.main.write_text('''fn main(): Unit {
    val point = addPoint(CRecordPoint(12, 1.5), CRecordPoint(-2, 0.75))
    print(point.x); print(point.y)
    print(counter()); counter(9); print(counter()); print(base())
    print(builtIn(35)); print(sumValues(2, -3, 2.5))
}''', encoding='utf-8')
        environment = {'PATH': ''}
        if WINDOWS:
            environment['SystemRoot'] = os.environ['SystemRoot']
        for mode in ([], ['--release']):
            result = self.command('run', self.root, *mode, environment=environment)
            self.assertEqual(result.stdout.splitlines(), ['10', '2.25', '5', '9', '11', '42', '1.5'])


if __name__ == '__main__':
    unittest.main()
