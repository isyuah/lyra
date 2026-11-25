# `std.log`

```lyra
import std.json.jsonInt
import std.json.jsonObject
import std.log.LogFormat
import std.log.Logger

val logger = Logger("worker", format: LogFormat.Json)
val result = logger.info("job completed", fields: jsonObject().put("job", jsonInt(7)).build())
```

`Logger` 提供 `debug`、`info`、`warn`、`error` 和通用 `log(level, message, fields)`；默认最低级别为 `Info`，默认输出为 `LogFormat.Text`。每条记录写入标准错误的一行，含 UTC 时间、级别、名称和消息；有任务 trace 时额外带上它。消息、字段等内容均转义，消息里的换行不会拆成多条日志。

`LogFormat.Json` 输出 JSON 对象，结构化字段放在可选的 `fields` 对象中。`fields` 必须是 `JsonValue` 对象，不能是数组或标量；调用者自行筛选可记录的字段，例如只传路由模板，不传带凭据的 URL。方法的 `Result<Unit, IoError>` 报告写出失败，包括日志管道断开；被级别过滤的记录不写出。
