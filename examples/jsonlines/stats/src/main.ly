import metrics.parseLine
import metrics.summary
import std.io.assert
import std.io.readLine
import std.io.writeError
import std.json.intText

fn main(): Unit {
    var lineNumber = 0
    var rows = 0
    var total = 0
    var rejected = 0
    while true {
        val line = readLine()
        if line.eof() { break }
        lineNumber += 1
        if !line.ok() {
            assert(line.invalidUtf8(), "cannot read stdin: errno " + intText(line.status))
            writeError("line " + intText(lineNumber) + ": input is not UTF-8\n")
            rejected += 1
            continue
        }
        match parseLine(line.value) {
            Ok(reading) => { rows += 1; total += reading.delta; },
            Err(message) => {
                writeError("line " + intText(lineNumber) + ": " + message + "\n")
                rejected += 1;
            },
        }
    }
    print(summary(rows, total, rejected))
}
