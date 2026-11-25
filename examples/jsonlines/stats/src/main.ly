import metrics.parseLine
import metrics.summary
import std.cli.Command
import std.cli.parseArgs
import std.config.Config
import std.config.intKey
import std.config.stringKey
import std.io.File
import std.io.IoError
import std.io.LineReader
import std.io.writeError
import std.jsonl.JsonlErrorKind
import std.jsonl.jsonlReader

fn aggregate(input: LineReader): Result<Unit,IoError> {
    val reader = jsonlReader(input, parseLine)
    var rows = 0
    var total = 0
    var rejected = 0
    while let Some(record) = reader.next()? {
        match record.value {
            Ok(reading) => { rows += 1; total += reading.delta; },
            Err(problem) => {
                val message = match problem.kind { JsonlErrorKind.Decode(_) => "invalid JSON reading", _ => problem.message() }
                writeError("line ${record.lineNumber}: ${message}\n")?
                rejected += 1;
            },
        }
    }
    print(summary(rows, total, rejected))
    Ok({})
}

fn fail(message: String, status: Int): Int {
    match writeError(message + "\n") {
        Ok(_) => status,
        Err(problem) => throw Exception(problem.message()),
    }
}

fn readInput(path: String, limit: Int): Result<Unit,IoError> {
    if path == "-" { return aggregate(LineReader.stdin(maxBytes: limit)?) }
    val file = File.openRead(path)?
    defer file.close()
    aggregate(LineReader.fromFile(file, maxBytes: limit)?)
}

fn main(): Int {
    val spec = Command("stats").option("--input").option("--config").option("--max-line-bytes")
    val arguments = match parseArgs(spec) {
        Ok(Run(command)) => command,
        Ok(Help(text)) => { print(text); return 0 },
        Err(problem) => return fail(problem.message(), 2),
    }
    if arguments.positionals.size != 0 { return fail("stats does not take positional arguments", 2) }
    val config = match arguments.value("--config") {
        None => Config.empty(),
        Some(path) => match Config.load(path) {
            Ok(settings) => settings,
            Err(problem) => return fail(problem.message(), 2),
        },
    }
    val input = stringKey("input", "-", cliName: Some("--input"), envName: Some("LYRA_STATS_INPUT"))
    val maxBytes = intKey("maxLineBytes", 1048576, cliName: Some("--max-line-bytes"), envName: Some("LYRA_STATS_MAX_LINE_BYTES"))
    val path = match config.get(input, Some(arguments)) {
        Ok(value) => value,
        Err(problem) => return fail(problem.message(), 2),
    }
    val limit = match config.get(maxBytes, Some(arguments)) {
        Ok(value) => value,
        Err(problem) => return fail(problem.message(), 2),
    }
    if limit <= 0 { return fail("maxLineBytes must be positive", 2) }
    match readInput(path, limit) {
        Ok(_) => 0,
        Err(problem) => {
            val source = if path == "-" { "stdin" } else { "input" }
            fail("cannot read ${source}: ${problem.message()}", 1)
        },
    }
}
