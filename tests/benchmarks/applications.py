import argparse
import asyncio
from contextlib import contextmanager
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import platform
from queue import Empty, Queue
import shutil
from statistics import median
import subprocess
import tempfile
from threading import Thread
from time import perf_counter


BODY = b'{"message":"hello","value":42}'
LYRA_HTTP = '''
import std.http.{HttpServerOptions, Request, Response, Router, Shutdown, listen}
import std.io.{readLine}

async fn main(): Unit {
    val listener = match listen(0, options: HttpServerOptions(capacity: 64)) {
        Ok(value) => value, Err(error) => throw Exception(error.message()),
    }
    val shutdown = Shutdown()
    val router = Router()
    router.get("/data", async (request: Request) => {
        Response.bytes(200, "application/json", "{\\"message\\":\\"hello\\",\\"value\\":42}".toBytes())
    })
    spawn blocking { val _ = readLine(); shutdown.request() };
    print(listener.port())
    await router.serve(listener, Some(shutdown))
}
'''
GO_HTTP = '''package main

import (
    "bufio"
    "context"
    "fmt"
    "net"
    "net/http"
    "os"
    "time"
)

func main() {
    listener, err := net.Listen("tcp4", "127.0.0.1:0")
    if err != nil { panic(err) }
    capacity := make(chan struct{}, 64)
    server := &http.Server{
        ReadHeaderTimeout: 30 * time.Second,
        IdleTimeout: 5 * time.Second,
        Handler: http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
            capacity <- struct{}{}
            defer func() { <-capacity }()
            if request.Method != "GET" || request.URL.Path != "/data" {
                http.NotFound(response, request)
                return
            }
            response.Header().Set("Content-Type", "application/json")
            fmt.Fprint(response, "{\\"message\\":\\"hello\\",\\"value\\":42}")
        }),
    }
    go func() {
        bufio.NewReader(os.Stdin).ReadString('\\n')
        server.Shutdown(context.Background())
    }()
    fmt.Println(listener.Addr().(*net.TCPAddr).Port)
    if err := server.Serve(listener); err != http.ErrServerClosed { panic(err) }
}
'''


def positive(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError('expected a positive integer')
    return number


def distribution(samples):
    ordered = sorted(samples)
    return {'samples': len(samples), 'p50': median(ordered), 'p95': ordered[math.ceil(len(ordered) * 0.95) - 1],
            'min': ordered[0], 'max': ordered[-1]}


def command(arguments, environment, timeout):
    start = perf_counter()
    result = subprocess.run(arguments, capture_output=True, env=environment, timeout=timeout)
    elapsed = (perf_counter() - start) * 1000
    if result.returncode:
        raise ValueError(f'{arguments[0]} exited with {result.returncode}: {result.stderr.decode(errors="replace")}')
    return elapsed, result


def memory(process):
    if os.name == 'nt':
        class Counters(ctypes.Structure):
            _fields_ = [('size', ctypes.c_ulong), ('faults', ctypes.c_ulong),
                        *[(name, ctypes.c_size_t) for name in ('peakRss', 'rss', 'peakPaged', 'paged', 'peakNonPaged',
                                                             'nonPaged', 'pagefile', 'peakPagefile', 'private')]]

        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        psapi = ctypes.WinDLL('psapi', use_last_error=True)
        psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(Counters), ctypes.c_ulong]
        handle = kernel.OpenProcess(0x410, False, process.pid)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            counters = Counters()
            counters.size = ctypes.sizeof(counters)
            if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.size):
                raise ctypes.WinError(ctypes.get_last_error())
            return {'rssBytes': counters.rss, 'peakRssBytes': counters.peakRss}
        finally:
            kernel.CloseHandle(handle)
    fields = dict(line.split(':', 1) for line in Path(f'/proc/{process.pid}/status').read_text().splitlines())
    return {'rssBytes': int(fields['VmRSS'].split()[0]) * 1024, 'peakRssBytes': int(fields['VmHWM'].split()[0]) * 1024}


@contextmanager
def server(executable, environment, timeout):
    with subprocess.Popen([executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          env=environment) as process:
        lines = Queue()
        reader = Thread(target=lambda: lines.put(process.stdout.readline()), daemon=True)
        reader.start()
        try:
            try:
                line = lines.get(timeout=timeout)
            except Empty as error:
                raise TimeoutError('HTTP server did not report its listening port') from error
            if not line:
                raise ValueError(f'HTTP server exited before listening: {process.stderr.read().decode(errors="replace")}')
            yield process, int(line)
            process.stdin.write(b'stop\n')
            process.stdin.flush()
            _, error = process.communicate(timeout=timeout)
            if process.returncode or error:
                raise ValueError(f'HTTP server shutdown failed: {process.returncode}: {error.decode(errors="replace")}')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=timeout)
            reader.join(timeout=timeout)


async def http_load(port, connections, requests, warmups, timeout):
    ready = asyncio.Queue()
    begin = asyncio.Event()
    request = b'GET /data HTTP/1.1\r\nHost: localhost\r\n\r\n'

    async def worker():
        reader, writer = await asyncio.open_connection('127.0.0.1', port)
        try:
            async def exchange():
                writer.write(request)
                await writer.drain()
                head = await reader.readuntil(b'\r\n\r\n')
                fields = head.split(b'\r\n')
                if fields[0].split()[1] != b'200':
                    raise ValueError(f'unexpected HTTP response: {head!r}')
                lengths = [int(field.split(b':', 1)[1]) for field in fields[1:] if field.lower().startswith(b'content-length:')]
                if lengths != [len(BODY)] or await reader.readexactly(lengths[0]) != BODY:
                    raise ValueError('HTTP benchmark produced a different response body')

            for _ in range(warmups):
                await exchange()
            await ready.put(True)
            await begin.wait()
            samples = []
            for _ in range(requests):
                start = perf_counter()
                await exchange()
                samples.append((perf_counter() - start) * 1000)
            return samples
        finally:
            writer.close()
            await writer.wait_closed()

    async with asyncio.timeout(timeout):
        workers = [asyncio.create_task(worker()) for _ in range(connections)]
        all_ready = asyncio.create_task(ready.get())
        try:
            for _ in range(connections):
                done, _ = await asyncio.wait([all_ready, *workers], return_when=asyncio.FIRST_COMPLETED)
                for result in done:
                    result.result()
                await all_ready
                all_ready = asyncio.create_task(ready.get())
            start = perf_counter()
            begin.set()
            samples = [sample for result in await asyncio.gather(*workers) for sample in result]
            elapsed = perf_counter() - start
            return {'requests': len(samples), 'seconds': elapsed, 'requestsPerSecond': len(samples) / elapsed,
                    'latencyMs': distribution(samples)}
        finally:
            all_ready.cancel()
            for task in workers:
                task.cancel()
            await asyncio.gather(all_ready, *workers, return_exceptions=True)


def main():
    parser = argparse.ArgumentParser(description='Compare native Lyra and Go application startup, memory, builds, and HTTP/1 keep-alive.')
    parser.add_argument('compiler', type=Path)
    parser.add_argument('--go', default=shutil.which('go'), help='Go compiler, required for the comparison')
    parser.add_argument('--runs', type=positive, default=10)
    parser.add_argument('--http-runs', type=positive, default=3, help='fresh server processes per connection count')
    parser.add_argument('--requests', type=positive, default=200, help='measured requests per connection')
    parser.add_argument('--connections', nargs='+', type=positive, default=[1, 16, 64])
    parser.add_argument('--warmups', type=positive, default=10)
    parser.add_argument('--functions', type=positive, default=200)
    parser.add_argument('--timeout', type=positive, default=120)
    parser.add_argument('--output', type=Path, help='write the measured JSON report to this file')
    options = parser.parse_args()
    if not options.go or max(options.connections) > 64 or options.requests + options.warmups > 1000:
        parser.error('provide --go, 1..64 connections, and at most 1000 total requests per connection')
    report = {'platform': platform.platform(), 'machine': platform.machine(), 'cpuCount': os.cpu_count(),
              'pythonVersion': platform.python_version(),
              'configuration': {'runs': options.runs, 'httpRuns': options.http_runs, 'requestsPerConnection': options.requests,
                                'connections': options.connections, 'warmups': options.warmups, 'functions': options.functions},
              'languages': {}}
    try:
        compiler_image = options.compiler.resolve()
        if compiler_image.with_name(compiler_image.name + '.real').is_file():
            compiler_image = compiler_image.with_name(compiler_image.name + '.real')
        with compiler_image.open('rb') as binary:
            report['compilerSha256'] = hashlib.file_digest(binary, 'sha256').hexdigest()
        with tempfile.TemporaryDirectory(prefix='lyra-app-bench-') as temporary:
            root = Path(temporary)
            suffix = '.exe' if os.name == 'nt' else ''
            environment = {**os.environ, 'LYRA_CACHE': str(root / 'cache'), 'GOCACHE': str(root / 'go-cache'),
                           'GOPROXY': 'off', 'GOTOOLCHAIN': 'local', 'CGO_ENABLED': '0'}
            for language in ('lyra', 'go'):
                package = root / language
                package.mkdir()
                executable = package / ('startup' + suffix)
                http = package / ('http' + suffix)
                if language == 'lyra':
                    (package / 'src').mkdir()
                    (package / 'lyra.json5').write_text('{name:"bench/app",version:"0.1.0"}', encoding='utf-8')
                    source = package / 'src/main.ly'
                    declarations = ''.join(f'fn value{index}(input: Int): Int {{ input + {index} }}\n' for index in range(options.functions))
                    code = declarations + 'fn main(): Unit { print(value0(42)) }\n'
                    build = [str(options.compiler.resolve()), 'build', str(package), '--release', '--offline', '-o']
                    version = subprocess.check_output([options.compiler.resolve(), '--version'], text=True).strip()
                else:
                    source = package / 'main.go'
                    declarations = ''.join(f'func value{index}(input int64) int64 {{ return input + {index} }}\n' for index in range(options.functions))
                    code = 'package main\nimport "fmt"\n' + declarations + 'func main() { fmt.Println(value0(42)) }\n'
                    build = [str(options.go), 'build', '-trimpath', '-o']
                    version = subprocess.check_output([options.go, 'version'], text=True).strip()
                source.write_text(code, encoding='utf-8')
                arguments = [*build, str(executable), *([str(source)] if language == 'go' else [])]
                cold, _ = command(arguments, environment, options.timeout)
                unchanged, _ = command(arguments, environment, options.timeout)
                source.write_text(code.replace('input + 0', 'input + 1'), encoding='utf-8')
                changed, _ = command(arguments, environment, options.timeout)
                restored, _ = command(arguments, environment, options.timeout)
                startup = []
                for index in range(options.runs + options.warmups):
                    elapsed, result = command([executable], environment, options.timeout)
                    if result.stdout != b'43\n' or result.stderr:
                        raise ValueError(f'{language} startup returned an unexpected value: {result.stdout!r} {result.stderr!r}')
                    if index >= options.warmups:
                        startup.append(elapsed)
                source.write_text(LYRA_HTTP if language == 'lyra' else GO_HTTP, encoding='utf-8')
                http_arguments = [*build, str(http), *([str(source)] if language == 'go' else [])]
                command(http_arguments, environment, options.timeout)
                result = {'version': version, 'binaryBytes': executable.stat().st_size, 'httpBinaryBytes': http.stat().st_size,
                          'buildMs': {'cold': cold, 'unchanged': unchanged, 'bodyEdit': changed, 'unchangedAfterEdit': restored},
                          'startupMs': distribution(startup), 'http': []}
                for connections in options.connections:
                    measurements = []
                    for _ in range(options.http_runs):
                        with server(http, environment, options.timeout) as (process, port):
                            before = memory(process)
                            load = asyncio.run(http_load(port, connections, options.requests, options.warmups, options.timeout))
                            measurements.append({'memoryBefore': before, 'memoryAfter': memory(process), **load})
                    result['http'].append({'connections': connections, 'runs': measurements,
                                           'requestsPerSecond': distribution([item['requestsPerSecond'] for item in measurements])})
                report['languages'][language] = result
        text = json.dumps(report, ensure_ascii=False, indent=2) + '\n'
        if options.output:
            options.output.parent.mkdir(parents=True, exist_ok=True)
            options.output.write_text(text, encoding='utf-8')
        print(text, end='')
    except (OSError, ValueError, subprocess.SubprocessError, TimeoutError, asyncio.IncompleteReadError) as error:
        parser.exit(1, f'benchmark failed: {error}\n')


if __name__ == '__main__':
    main()
