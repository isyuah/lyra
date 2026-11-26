import contextlib
import os
import queue
import re
import subprocess
import threading


WINDOWS = os.name == 'nt'
CLANG = 'clang' if WINDOWS else 'clang-18'
LINKER = 'lld-link.exe' if WINDOWS else 'ld.lld'


def isolated_environment(environment):
    tool_variables = {'PATH', 'LIB', 'INCLUDE', 'LIBPATH', 'LIBRARY_PATH', 'LINK', '_LINK_', 'LD_LIBRARY_PATH', 'LD_PRELOAD'}
    result = {key: value for key, value in environment.items() if key.upper() not in tool_variables}
    result['PATH'] = ''
    return result


def executable(path):
    return path.with_name(path.name + '.exe') if WINDOWS else path


def c_archive(source, output, *options):
    target = output.with_suffix('.obj' if WINDOWS else '.o')
    archive = output.with_suffix('.lib' if WINDOWS else '.a')
    runtime = ['-fms-runtime-lib=static'] if WINDOWS else ['-pthread']
    subprocess.run([CLANG, '-std=c11', *runtime, *options, '-c', source, '-o', target], check=True, timeout=30)
    subprocess.run(['llvm-lib', f'/OUT:{archive}', target] if WINDOWS else ['ar', 'rcs', archive, target], check=True, timeout=30)
    return archive


def assert_linkage(test, program, *linux_libraries):
    if WINDOWS:
        output = subprocess.check_output(['llvm-readobj', '--coff-imports', program], text=True)
        test.assertIn('Format: COFF-x86-64', output)
        libraries = {name.lower() for name in re.findall(r'^\s*Name:\s+(\S+\.dll)\s*$', output, re.I | re.M)}
        system_libraries = {
            'advapi32.dll', 'bcrypt.dll', 'crypt32.dll', 'dbghelp.dll', 'gdi32.dll', 'iphlpapi.dll', 'kernel32.dll',
            'ntdll.dll', 'ole32.dll', 'psapi.dll', 'secur32.dll', 'shell32.dll', 'user32.dll', 'userenv.dll', 'ws2_32.dll',
        }
        test.assertIn('kernel32.dll', libraries)
        test.assertEqual({name for name in libraries - system_libraries if not name.startswith('api-ms-win-crt-')}, set())
    else:
        output = subprocess.check_output(['readelf', '-dW', program], text=True)
        test.assertEqual(set(re.findall(r'\(NEEDED\).*?\[(.*?)\]', output)), {'libc.so.6', *linux_libraries})


class ProcessLines:
    def __init__(self, stream):
        self.pending = queue.Queue()
        self.reader = threading.Thread(target=self.read, args=(stream,), daemon=True)
        self.reader.start()

    def read(self, stream):
        for line in stream:
            self.pending.put(line)
        self.pending.put(b'')

    def line(self, timeout=8):
        try:
            return self.pending.get(timeout=timeout)
        except queue.Empty as error:
            raise TimeoutError('application did not print the expected line') from error

    def remaining(self):
        pieces = []
        while line := self.line():
            pieces.append(line)
        return b''.join(pieces)


@contextlib.contextmanager
def running(arguments, **options):
    process = subprocess.Popen(arguments, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **options)
    output = ProcessLines(process.stdout)
    try:
        yield process, output
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        output.reader.join(timeout=5)
        process.stdin.close()
        process.stdout.close()
        process.stderr.close()
