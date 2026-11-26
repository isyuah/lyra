import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
from queue import Empty, Queue
import subprocess
import tempfile
from threading import Thread
from time import perf_counter

from applications import command, distribution, positive


@contextmanager
def language_server(compiler, environment, timeout):
    with subprocess.Popen([compiler, 'lsp', '--stdio'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=environment) as process:
        incoming = Queue()

        def read():
            try:
                for header in iter(process.stdout.readline, b''):
                    length = int(header.split(b':', 1)[1])
                    if process.stdout.readline() != b'\r\n':
                        raise ValueError('invalid LSP message framing')
                    incoming.put(json.loads(process.stdout.read(length)))
            except (ValueError, OSError) as error:
                incoming.put(error)
            finally:
                incoming.put(EOFError('LSP server closed stdout'))

        reader = Thread(target=read, daemon=True)
        reader.start()

        def send(method, params=None, number=None):
            value = {'jsonrpc': '2.0', 'method': method, 'params': params or {}}
            if number is not None:
                value['id'] = number
            body = json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
            process.stdin.write(f'Content-Length: {len(body)}\r\n\r\n'.encode() + body)
            process.stdin.flush()

        def receive():
            try:
                value = incoming.get(timeout=timeout)
            except Empty as error:
                raise TimeoutError('LSP response exceeded the benchmark timeout') from error
            if isinstance(value, Exception):
                raise value
            if value.get('error'):
                raise ValueError(f'LSP request failed: {value["error"]}')
            return value

        try:
            yield send, receive
            send('shutdown', number=99)
            while receive().get('id') != 99:
                pass
            send('exit')
            _, error = process.communicate(timeout=timeout)
            if process.returncode or error:
                raise ValueError(f'LSP shutdown failed: {process.returncode}: {error.decode(errors="replace")}')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=timeout)
            reader.join(timeout=timeout)


def workspace(root, count, functions):
    app = root / 'app'
    (app / 'src').mkdir(parents=True)
    dependencies = {}
    imports = []
    calls = []
    for index in range(count):
        package = root / f'lib{index}'
        (package / 'src').mkdir(parents=True)
        name = f'bench/lib{index}'
        (package / 'lyra.json5').write_text(json.dumps({'name': name, 'version': '0.1.0', 'kind': 'lib'}), encoding='utf-8')
        source = ''.join(f'pub fn compute{number}(value: Int): Int {{ val result = value + {number}; result }}\n'
                         for number in range(functions))
        (package / 'src/api.ly').write_text(source, encoding='utf-8')
        dependencies[f'lib{index}'] = {'name': name, 'version': '^0.1.0', 'path': f'../lib{index}'}
        imports.append(f'import lib{index} as lib{index}\n')
        calls.append(f'    print(lib{index}.compute0(42))\n')
    (app / 'lyra.json5').write_text(json.dumps({'name': 'bench/app', 'version': '0.1.0', 'dependencies': dependencies}), encoding='utf-8')
    entry = app / 'src/main.ly'
    entry.write_text(''.join(imports) + 'fn main(): Unit {\n' + ''.join(calls) + '}\n', encoding='utf-8')
    return app, entry, root / 'lib0/src/api.ly'


def measure(compiler, root, count, functions, runs, warmups, timeout):
    app, entry, library = workspace(root, count, functions)
    environment = {**os.environ, 'LYRA_CACHE': str(root / 'cache')}
    baseline_ms, _ = command([compiler, 'check', app, '--offline'], environment, timeout)
    target = library.as_uri()
    initial = library.read_text(encoding='utf-8')
    samples = []
    with language_server(compiler, environment, timeout) as (send, receive):
        send('initialize', {'capabilities': {}, 'workspaceFolders': [{'uri': app.as_uri(), 'name': 'benchmark'}]}, number=1)
        if receive().get('id') != 1:
            raise ValueError('unexpected initialize response')
        send('initialized')

        def diagnostics(uri, version):
            while True:
                value = receive()
                if value.get('method') != 'textDocument/publishDiagnostics':
                    continue
                params = value['params']
                if params['uri'] == uri and params.get('version') == version:
                    if params['diagnostics']:
                        raise ValueError(f'valid benchmark source received diagnostics: {params["diagnostics"]}')
                    return

        for path in (entry, library):
            send('textDocument/didOpen', {'textDocument': {'uri': path.as_uri(), 'languageId': 'lyra', 'version': 1,
                                                         'text': path.read_text(encoding='utf-8')}})
            diagnostics(path.as_uri(), 1)
        for index in range(warmups + runs):
            text = initial.replace('value + 0;', f'value + {index + 1};', 1)
            start = perf_counter()
            send('textDocument/didChange', {'textDocument': {'uri': target, 'version': index + 2}, 'contentChanges': [{'text': text}]})
            diagnostics(target, index + 2)
            elapsed = (perf_counter() - start) * 1000
            if index >= warmups:
                samples.append(elapsed)
    library.write_text(text, encoding='utf-8')
    command([compiler, 'check', app, '--offline'], environment, timeout)
    return {'packages': count + 1, 'functions': count * functions + 1, 'openDocuments': 2,
            'fullCheckMs': baseline_ms, 'editToDiagnosticsMs': distribution(samples)}


def main():
    parser = argparse.ArgumentParser(description='Measure Lyra multi-package LSP edit-to-diagnostic latency, including debounce.')
    parser.add_argument('compiler', type=Path)
    parser.add_argument('--packages', nargs='+', type=positive, default=[2, 8, 24], help='number of library dependencies')
    parser.add_argument('--functions', type=positive, default=40, help='functions per dependency')
    parser.add_argument('--runs', type=positive, default=10)
    parser.add_argument('--warmups', type=positive, default=2)
    parser.add_argument('--timeout', type=positive, default=120)
    parser.add_argument('--output', type=Path)
    options = parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='lyra-lsp-bench-') as temporary:
            report = {'version': subprocess.check_output([options.compiler.resolve(), '--version'], text=True).strip(),
                      'cases': [measure(options.compiler.resolve(), Path(temporary) / str(count), count, options.functions,
                                        options.runs, options.warmups, options.timeout) for count in options.packages]}
        text = json.dumps(report, ensure_ascii=False, indent=2) + '\n'
        if options.output:
            options.output.parent.mkdir(parents=True, exist_ok=True)
            options.output.write_text(text, encoding='utf-8')
        print(text, end='')
    except (OSError, ValueError, EOFError, TimeoutError, subprocess.SubprocessError) as error:
        parser.exit(1, f'benchmark failed: {error}\n')


if __name__ == '__main__':
    main()
