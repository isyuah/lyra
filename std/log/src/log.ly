import std.io.{IoError, writeError}
import std.json.{JsonValue, jsonObject, jsonString, quoteJson}
import std.task.{trace}
import std.time.{Clock}

/// 日志严重程度，从 `Debug` 到 `Error` 逐级升高。
pub enum LogLevel {
    /// 用于调试细节的最低级别，默认不输出。
    Debug,
    /// 普通运行事件，也是默认输出阈值。
    Info,
    /// 需要关注、但应用仍可继续处理的情况。
    Warn,
    /// 用于报告失败的最高级别。
    Error,
}

/// 每条标准错误日志采用的单行格式。
pub enum LogFormat {
    /// 带引号转义的可读文本，适合直接查看。
    Text,
    /// 每条日志是一个 JSON 对象，便于机器解析。
    Json,
}

fn levelName(level: LogLevel): String {
    match level { Debug => "DEBUG", Info => "INFO", Warn => "WARN", Error => "ERROR" }
}

fn levelRank(level: LogLevel): Int {
    match level { Debug => 0, Info => 1, Warn => 2, Error => 3 }
}

/// 向标准错误写日志；每行包含 UTC 时间、级别、记录器名称、消息和可选任务追踪标识。
pub class Logger(
    /// 标在每条日志里的来源名称。
    pub val name: String,
    /// 最低输出级别，低于阈值的记录被丢弃。
    pub val minimum: LogLevel = LogLevel.Info,
    /// 写出的单行编码格式。
    pub val format: LogFormat = LogFormat.Text
) {
    /// 写入一条指定级别的日志。`fields` 给出时须是 JSON 对象；写出失败返回 I/O 错误。
    pub fn log(level: LogLevel, message: String, fields: JsonValue? = None): Result<Unit, IoError> {
        if levelRank(level) < levelRank(minimum) { return Ok({}) }
        if let Some(data) = fields {
            if !data.isObject() { throw Exception("log fields must be a JSON object") }
        }
        val time = Clock.now().formatUtc()
        val label = levelName(level)
        val currentTrace = trace()
        val line = match format {
            Text => {
                var output = "${time} ${label} ${quoteJson(name)} ${quoteJson(message)}"
                if currentTrace != "" { output += " trace=" + quoteJson(currentTrace) }
                if let Some(data) = fields { output += " fields=" + data.encode() }
                output
            },
            Json => {
                val object = jsonObject().put("time", jsonString(time)).put("level", jsonString(label))
                    .put("logger", jsonString(name)).put("message", jsonString(message))
                if currentTrace != "" { object.put("trace", jsonString(currentTrace)); }
                if let Some(data) = fields { object.put("fields", data); }
                object.build().encode()
            },
        }
        writeError(line + "\n")
    }

    /// 写入调试日志，默认级别阈值会过滤这条记录。
    pub fn debug(message: String, fields: JsonValue? = None): Result<Unit, IoError> { log(LogLevel.Debug, message, fields) }
    /// 按 `Info` 级别记录普通运行事件。
    pub fn info(message: String, fields: JsonValue? = None): Result<Unit, IoError> { log(LogLevel.Info, message, fields) }
    /// 按 `Warn` 级别记录警告。
    pub fn warn(message: String, fields: JsonValue? = None): Result<Unit, IoError> { log(LogLevel.Warn, message, fields) }
    /// 按 `Error` 级别记录失败。
    pub fn error(message: String, fields: JsonValue? = None): Result<Unit, IoError> { log(LogLevel.Error, message, fields) }
}
