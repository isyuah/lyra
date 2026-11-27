import std.io.{IoError, IoErrorKind, LineReader}
import std.json.{JsonCodec}

/// 单条记录的失败原因；与无法继续读流的 I/O 错误分开。
pub enum JsonlErrorKind {
    /// 行内容不是有效 UTF-8。
    InvalidUtf8,
    /// 此行超出输入读取器的字节上限。
    LineTooLong,
    /// 解码器拒绝该行，载荷为返回的诊断文本。
    Decode(String),
}

/// 某一行未能解码的结构化错误，不阻止读取下一行。
pub struct JsonlError(
    /// 用于区分行编码、行长和解码问题的分类。
    pub val kind: JsonlErrorKind
) {
    /// 返回适合日志或终端显示的失败详情，不含行号。
    pub fn message(): String {
        match kind {
            InvalidUtf8 => "input is not UTF-8",
            LineTooLong => "line exceeds its byte limit",
            Decode(detail) => detail,
        }
    }
}

/// 一条带物理行号的 JSONL 解码结果。
pub struct JsonlRow<T>(
    /// 从 `1` 开始的物理行号，空行与坏行也计数。
    pub val lineNumber: Int,
    /// 解码成功的值或这一行自身的错误。
    pub val value: Result<T, JsonlError>
)

/// 逐行借用 `LineReader` 解码，不缓存输入文件，也不接管其生命周期。
pub class JsonlReader<T>(val input: LineReader, val decode: (String) -> Result<T,String>, var lineNumber: Int = 0) {
    fn record(value: Result<T, JsonlError>): Result<JsonlRow<T>?, IoError> {
        lineNumber += 1
        Ok(Some(JsonlRow(lineNumber, value)))
    }

    /// 返回下一物理行，空行也交给解码器。EOF 为 `Ok(None)`；坏行作为 `row.value`，读流失败作为外层 `Err`。
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

/// 创建使用自定义解码函数的读取器，每次给解码函数传入不含换行符的一行。
pub fn jsonlReader<T>(input: LineReader, decode: (String) -> Result<T,String>): JsonlReader<T> {
    JsonlReader(input, decode)
}

/// 使用同一个 JSON codec 解码每行独立的 JSON 值。
pub fn jsonlReader<T>(input: LineReader, codec: JsonCodec<T>): JsonlReader<T> {
    JsonlReader<T>(input, (line) => { codec.decode(line) })
}
