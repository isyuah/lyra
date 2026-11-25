from datetime import datetime
import errno
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from platform_cli import executable, running


COMPILER = Path(sys.argv.pop(1)).resolve()


class ApplicationStd(unittest.TestCase):
    def programs(self, source):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        package = Path(temporary.name)
        (package / 'src').mkdir()
        (package / 'lyra.json5').write_text('{name:"test/application-std",version:"0.1.0"}', encoding='utf-8')
        (package / 'src/main.ly').write_text(source, encoding='utf-8')
        for mode, flags in (('debug', []), ('release', ['--release'])):
            with self.subTest(mode=mode):
                app = executable(package / mode)
                result = subprocess.run([COMPILER, 'build', package, *flags, '-o', app], capture_output=True, timeout=40)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
            if result.returncode == 0:
                yield package, app

    def run_app(self, app, *arguments, **options):
        result = subprocess.run([app, *arguments], capture_output=True, timeout=10, **options)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        return result

    def test_jsonl_streams_stdin_and_file_with_recoverable_line_errors(self):
        for package, app in self.programs('''
import std.io.{File}
import std.io.{IoError}
import std.io.{LineReader}
import std.json.{jsonCodecInt}
import std.jsonl.{JsonlErrorKind}
import std.jsonl.{jsonlReader}
import std.os.{args}

fn read(input: LineReader): Result<Unit, IoError> {
    val reader = jsonlReader(input, jsonCodecInt())
    while let Some(row) = reader.next()? {
        val label = match row.value {
            Ok(value) => value.toString(),
            Err(error) => match error.kind { InvalidUtf8 => "utf8", LineTooLong => "limit", Decode(_) => "decode" },
        }
        print("${row.lineNumber}: ${label}")
    }
    Ok({})
}

fn run(): Result<Unit, IoError> {
    val arguments = args()?
    if arguments.size == 0 { return read(LineReader.stdin(maxBytes: 8)?) }
    val file = File.openRead(arguments[0])?
    val input = LineReader.fromFile(file, maxBytes: 8)?
    read(input)?
    file.close()
    match jsonlReader(input, jsonCodecInt()).next() {
        Err(problem) => print(problem.message()),
        Ok(_) => throw Exception("closed input looked like EOF or a record"),
    }
    Ok({})
}

fn main(): Unit { match run() { Ok(_) => {}, Err(problem) => throw Exception(problem.message()) } }
'''):
            data = b'1\r\n\xff\n123456789\n\n{"x":1}\n2'
            expected = b'1: 1\n2: utf8\n3: limit\n4: decode\n5: decode\n6: 2\n'
            result = self.run_app(app, input=data)
            self.assertEqual((result.stdout, result.stderr), (expected, b''))
            path = package / '记录 🙂.jsonl'
            path.write_bytes(data)
            result = self.run_app(app, path)
            self.assertEqual((result.stdout, result.stderr), (expected + b'readLine: file is closed\n', b''))

    def test_cli_uses_actual_arguments_and_reports_bad_options(self):
        for _, app in self.programs('''
import std.cli.{Command}
import std.cli.{parseArgs}

fn main(): Unit {
    val command = Command("stats").option("--input", required: true).option("--limit").flag("--verbose")
    match parseArgs(command) {
        Ok(Run(parsed)) => {
            print(match parsed.value("--input") { None => "absent", Some(value) => "[${value}]" })
            print(match parsed.value("--limit") { None => "absent", Some(value) => "[${value}]" })
            print(parsed.flag("--verbose"))
            for item in parsed.positionals { print(item) }
        },
        Ok(Help(usage)) => print(usage),
        Err(error) => print(error.message()),
    }
}
'''):
            for arguments, output in (
                (('before', '--input=中🙂=rest', '--limit', '-7', '--verbose', '-', '--', '--literal'), '[中🙂=rest]\n[-7]\ntrue\nbefore\n-\n--literal\n'),
                (('--input=',), '[]\nabsent\nfalse\n'),
                (('--input', 'with spaces'), '[with spaces]\nabsent\nfalse\n'),
                (('--verbose',), 'missing required option --input\n'),
                (('--input', '--verbose'), '--input requires a value\n'),
                (('--verbose=1',), '--verbose does not take a value\n'),
                (('--input=one', '--input=two'), '--input was supplied more than once\n'),
                (('--unknown',), 'unknown option --unknown\n'),
                (('--input=one', '--', '--help'), '[one]\nabsent\nfalse\n--help\n'),
                (('--help',), 'Usage: stats [options] [--] [args...]\n\nOptions:\n  --input <value> (required)\n  --limit <value>\n  --verbose\n  --help\n\n'),
            ):
                with self.subTest(arguments=arguments):
                    result = self.run_app(app, *arguments)
                    self.assertEqual((result.stdout.decode(), result.stderr), (output, b''))

    def test_config_precedence_and_source_without_secret(self):
        for package, app in self.programs('''
import std.cli.{Command}
import std.cli.{parseArgs}
import std.config.{Config}
import std.config.{boolKey}
import std.config.{configKey}
import std.config.{intKey}
import std.config.{stringKey}
import std.toml.{tomlCodecInt}

fn main(): Unit {
    val arguments = match parseArgs(Command("config").option("--port").option("--mode").flag("--enabled")) {
        Ok(Run(values)) => values,
        _ => throw Exception("expected parsed arguments"),
    }
    val config = match Config.load(arguments.positionals[0]) {
        Ok(settings) => settings,
        Err(error) => throw Exception(error.message()),
    }
    val port = intKey("server.port", 9000, cliName: Some("--port"), envName: Some("LYRA_STD_TEST_PORT"))
    print(match config.get(port, Some(arguments)) { Ok(value) => value.toString(), Err(error) => error.message() })
    print(match config.get(intKey("server.missing", -1)) { Ok(value) => value.toString(), Err(error) => error.message() })
    print(match config.get(intKey("server.title", 0)) { Ok(value) => value.toString(), Err(error) => error.message() })
    print(match config.get(stringKey("server.title", "default", envName: Some("LYRA_STD_TEST_EMPTY"))) {
        Ok(value) => "[${value}]", Err(error) => error.message(),
    })
    val enabled = boolKey("server.enabled", true, cliName: Some("--enabled"))
    print(match config.get(enabled, Some(arguments)) { Ok(value) => value, Err(error) => throw Exception(error.message()) })
    val mode = configKey("server.mode", tomlCodecInt(), (text) => {
        if text == "proxy" { Ok(1) } else { Err("received ${text}") }
    }, 0, cliName: Some("--mode"))
    print(match config.get(mode, Some(arguments)) { Ok(value) => value.toString(), Err(error) => error.message() })
}
'''):
            path = package / '配置 🙂.toml'
            path.write_text('[server]\nport = 80\ntitle = "TOML 中"\nenabled = false\nmode = 2\n', encoding='utf-8')
            environment = {**os.environ, 'LYRA_STD_TEST_EMPTY': ''}
            environment.pop('LYRA_STD_TEST_PORT', None)
            for cli, env, expected_port, expected_mode in (
                ((), {}, '80', '2'),
                ((), {'LYRA_STD_TEST_PORT': '8080'}, '8080', '2'),
                (('--port=8443', '--enabled', '--mode=proxy'), {'LYRA_STD_TEST_PORT': '8080'}, '8443', '1'),
                (('--port=private-data', '--mode=private-data'), {'LYRA_STD_TEST_PORT': '8080'},
                 'CLI --port: invalid configuration value', 'CLI --mode: invalid configuration value'),
                ((), {'LYRA_STD_TEST_PORT': 'private-data'}, 'env LYRA_STD_TEST_PORT: invalid configuration value', '2'),
            ):
                result = self.run_app(app, path, *cli, env={**environment, **env})
                self.assertEqual(result.stderr, b'')
                self.assertEqual(result.stdout.decode().splitlines(), [expected_port, '-1', f'{path}: server.title: invalid configuration value',
                                                                        '[]', 'true' if '--enabled' in cli else 'false', expected_mode])

    def test_log_emits_timestamp_trace_and_structured_fields_on_stderr(self):
        for _, app in self.programs(r'''
import std.io.{IoError}
import std.json.{jsonInt}
import std.json.{jsonObject}
import std.log.{LogFormat}
import std.log.{Logger}
import std.task.{withTrace}

fn check(result: Result<Unit, IoError>): Unit {
    match result { Ok(_) => {}, Err(error) => throw Exception(error.message()) }
}

async fn traced(): Unit {
    val logger = Logger("统计🙂", format: LogFormat.Json)
    check(logger.debug("filtered"))
    check(logger.info("first\nsecond", fields: jsonObject().put("delta", jsonInt(9007199254740993)).build()))
}

async fn main(): Unit {
    await withTrace("req-中", traced())
    check(Logger("text").warn("first\nsecond"))
}
'''):
            result = self.run_app(app)
            self.assertEqual(result.stdout, b'')
            structured, text = result.stderr.decode().splitlines()
            event = json.loads(structured)
            timestamp = event.pop('time')
            self.assertTrue(timestamp.endswith('Z'))
            datetime.fromisoformat(timestamp.replace('Z', '+00:00'))
            self.assertEqual(event, {'level': 'INFO', 'logger': '统计🙂', 'message': 'first\nsecond',
                                     'trace': 'req-中', 'fields': {'delta': 9007199254740993}})
            text_time, message = text.split(' ', 1)
            self.assertTrue(text_time.endswith('Z'))
            datetime.fromisoformat(text_time.replace('Z', '+00:00'))
            self.assertEqual(message, r'WARN "text" "first\nsecond"')

    @unittest.skipUnless(sys.platform.startswith('linux'), 'POSIX SIGPIPE and FIFO')
    def test_closed_pipe_returns_io_errors_for_log_and_buffered_file_writes(self):
        for package, app in self.programs('''
import std.io.{CloseError}
import std.io.{File}
import std.io.{IoError}
import std.io.{readLine}
import std.log.{Logger}
import std.os.{args}

fn require<T>(result: Result<T, IoError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

fn outcome(result: Result<Unit, IoError>): Unit {
    print(match result { Ok(_) => 0, Err(error) => error.code() ?? -1 })
}

fn main(): Unit {
    val arguments = require(args())
    if arguments.size == 0 {
        outcome(Logger("worker").info("disconnected log"))
        return
    }
    val sink = require(File.openWrite(arguments[0]))
    print("opened")
    val _ = require(readLine())
    outcome(sink.write(b"flush"))
    outcome(sink.flush())
    var payload = "x"
    for expansion in 0..<16 { payload += payload }
    outcome(sink.write(payload.toBytes()))
    outcome(sink.write(b"close"))
    print(try { sink.close(); false } catch _: CloseError { true })
}
'''):
            reader, writer = os.pipe()
            os.close(reader)
            with os.fdopen(writer, 'wb') as sink:
                result = subprocess.run([app], stdout=subprocess.PIPE, stderr=sink, timeout=10)
            self.assertEqual((result.returncode, result.stdout), (0, f'{errno.EPIPE}\n'.encode()))

            path = package / f'{app.name}.fifo'
            os.mkfifo(path)
            with os.fdopen(os.open(path, os.O_RDONLY | os.O_NONBLOCK), 'rb') as source:
                with running([app, path]) as (process, output):
                    self.assertEqual(output.line(), b'opened\n')
                    source.close()
                    process.stdin.write(b'go\n')
                    process.stdin.flush()
                    self.assertEqual(output.remaining(), f'0\n{errno.EPIPE}\n{errno.EPIPE}\n0\ntrue\n'.encode())
                    self.assertEqual(process.wait(timeout=10), 0, process.stderr.read())


if __name__ == '__main__':
    unittest.main()
