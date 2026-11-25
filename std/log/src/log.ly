import std.io.{IoError, writeError}
import std.json.{JsonValue, jsonObject, jsonString, quoteJson}
import std.task.{trace}
import std.time.{Clock}

pub enum LogLevel { Debug, Info, Warn, Error }
pub enum LogFormat { Text, Json }

fn levelName(level: LogLevel): String {
    match level { Debug => "DEBUG", Info => "INFO", Warn => "WARN", Error => "ERROR" }
}

fn levelRank(level: LogLevel): Int {
    match level { Debug => 0, Info => 1, Warn => 2, Error => 3 }
}

pub class Logger(pub val name: String, pub val minimum: LogLevel = LogLevel.Info, pub val format: LogFormat = LogFormat.Text) {
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

    pub fn debug(message: String, fields: JsonValue? = None): Result<Unit, IoError> { log(LogLevel.Debug, message, fields) }
    pub fn info(message: String, fields: JsonValue? = None): Result<Unit, IoError> { log(LogLevel.Info, message, fields) }
    pub fn warn(message: String, fields: JsonValue? = None): Result<Unit, IoError> { log(LogLevel.Warn, message, fields) }
    pub fn error(message: String, fields: JsonValue? = None): Result<Unit, IoError> { log(LogLevel.Error, message, fields) }
}
