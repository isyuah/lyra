# std.sqlite：绑定参数查询

`std.sqlite` 在 Linux 上使用 `libsqlite3.so.0`，Windows SDK 静态链接 SQLite。`Connection`、`Statement` 的底层句柄不公开。打开、准备、绑定、推进游标和读取都返回 `Result<T, DbError>`，可用 `?` 传播错误。位置 **1** 起始的 `bindInt` / `bindText` 绑定值；读列的索引从 **0** 开始。`next()` 返回 `Ok(true)` 表示一行可读，`Ok(false)` 表示结束；读取 SQL `NULL` 得到 `Ok(None)`，和 `Ok(Some(""))` 不同。

例如，读取已有 `notes.db` 中的一条留言：

```lyra
import std.sqlite.DbError
import std.sqlite.openReadOnly

fn lookup(id: Int): Result<String?, DbError> {
    val db = openReadOnly("notes.db")?
    defer db.close()
    val query = db.prepare("SELECT body FROM notes WHERE id = ?1")?
    defer query.close()
    query.bindInt(1, id)?
    if !query.next()? { return Ok(None) }
    query.readText(0)
}

fn main(): Unit {
    match lookup(7) {
        Ok(Some(body)) => print(body),
        Ok(None) => print("没有留言正文"),
        Err(error) => print("${error.operation}: ${error.message()}"),
    }
}
```

`DbError.kind` 区分 `Busy`（句柄正被占用）、`Closed`（句柄已关闭）、`Sqlite`（SQLite 返回失败）和 `Invalid`（例如非法下标或不匹配的列类型）。原生失败的 `code`、`operation` 和 `message()` 可用于日志；应用层也可以用 `DbError.typeMismatch(operation, message)` 报告额外的结果类型约束。`readText` 返回独立的 Lyra 字符串，关闭 statement 后仍可读取。

`close(): Unit` 是同步清理，先关 statement，再关连接。已有操作占用句柄、存在未关的 statement 或另一关闭尚未完成时，`close` 抛 `std.sqlite.Busy`，待操作结束可重试；其他关闭失败抛 `std.sqlite.CloseError`。一次关闭对全部别名生效，之后重复关闭幂等。已由 `next()` 返回的执行错误不会在 statement 释放后再被重复抛出。异步应用把整段查询与清理放进 `blocking`。
