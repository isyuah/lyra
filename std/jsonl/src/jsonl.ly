import std.io.{IoError, IoErrorKind, LineReader}
import std.json.{JsonCodec}

pub enum JsonlErrorKind { InvalidUtf8, LineTooLong, Decode(String) }

pub struct JsonlError(pub val kind: JsonlErrorKind) {
    pub fn message(): String {
        match kind {
            InvalidUtf8 => "input is not UTF-8",
            LineTooLong => "line exceeds its byte limit",
            Decode(detail) => detail,
        }
    }
}

pub struct JsonlRow<T>(pub val lineNumber: Int, pub val value: Result<T, JsonlError>)

pub class JsonlReader<T>(val input: LineReader, val decode: (String) -> Result<T,String>, var lineNumber: Int = 0) {
    fn record(value: Result<T, JsonlError>): Result<JsonlRow<T>?, IoError> {
        lineNumber += 1
        Ok(Some(JsonlRow(lineNumber, value)))
    }

    pub fn next(): Result<JsonlRow<T>?, IoError> {
        match input.readLine() {
            Ok(None) => Ok(None),
            Ok(Some(line)) => match decode(line) {
                Ok(value) => record(Ok(value)),
                Err(detail) => record(Err(JsonlError(JsonlErrorKind.Decode(detail)))),
            },
            Err(problem) => match problem.kind {
                IoErrorKind.InvalidUtf8 => record(Err(JsonlError(JsonlErrorKind.InvalidUtf8))),
                IoErrorKind.LineTooLong => record(Err(JsonlError(JsonlErrorKind.LineTooLong))),
                _ => Err(problem),
            },
        }
    }
}

pub fn jsonlReader<T>(input: LineReader, decode: (String) -> Result<T,String>): JsonlReader<T> {
    JsonlReader(input, decode)
}

pub fn jsonlReader<T>(input: LineReader, codec: JsonCodec<T>): JsonlReader<T> {
    JsonlReader<T>(input, (line) => { codec.decode(line) })
}
