import std.os.args
import std.io.IoError
import std.io.readFile
import std.io.readLine
import std.io.writeFile

fn copyLine(): Result<Unit, IoError> {
    val arguments = args()?
    val line = readLine()?
    #assert(arguments.size == 1 && line != None, "expected a path and one line")
    val path = arguments[0]
    writeFile(path, line ?? "")?
    print(readFile(path)?)
    Ok({})
}

fn main(): Unit {
    match copyLine() { Ok(_) => {}, Err(error) => throw Exception(error.message()) }
}
