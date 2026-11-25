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
        installed = subprocess.run([CMAKE, '--install', COMPILER.parent.parent, '--prefix', staged], capture_output=True, text=True, encoding='utf-8', timeout=60)
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


if __name__ == '__main__':
    unittest.main()
