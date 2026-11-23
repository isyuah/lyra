import std.io.arg
import std.io.assert
import std.io.readFile
import std.io.readLine
import std.io.writeFile

fn main(): Unit {
    val path = arg(0)
    val line = readLine()
    assert(path.ok() && line.ok(), "expected a path and one line")
    assert(writeFile(path.value, line.value) == 0, "write")
    val file = readFile(path.value)
    assert(file.ok(), "read")
    print(file.value)
}
