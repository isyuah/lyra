# std.sqlite：查询、写入和事务

`std.sqlite` 在 Windows 与 Linux 都由 SDK 静态链接 SQLite；应用不依赖额外安装 SQLite 动态库。打开、准备、执行、绑定、推进游标和读取都返回 `Result<T, DbError>`，可用 `?` 传播错误。参数索引从 **1** 开始，可绑定 `Int`、`String`、`Float64`、`Bytes` 或 `bindNull(position)`。读列索引从 **0** 开始，`readInt`、`readText`、`readFloat` 和 `readBlob` 各自检查 SQLite 的实际存储类型，不做隐式转型。`next()` 返回 `Ok(true)` 表示一行可读，`Ok(false)` 表示结束；SQL `NULL` 读作 `Ok(None)`，空字符串和空 BLOB 仍是 `Some`。`bindFloat` 拒绝会被 SQLite 当作 `NULL` 写入的 NaN。

例如，读取已有 `notes.db` 中的一条留言：

```lyra
import std.sqlite.{DbError}
import std.sqlite.{openReadOnly}

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

`DbError.kind` 区分 `Busy`（同连接被占用或 SQLite 报告锁冲突）、`Closed`（句柄已关闭）、`Sqlite`（其他 SQLite 失败）和 `Invalid`（例如非法下标或不匹配的列类型）。绑定和读列先确认句柄可用，再检查索引。原生失败的 `code`、`operation` 和 `message()` 可用于日志；应用层也可以用 `DbError.typeMismatch(operation, message)` 报告额外的结果类型约束。`readText` 与 `readBlob` 返回独立副本，关闭 statement 后仍可读取。

`close(): Unit` 是同步清理，先关 statement，再关连接。已有操作占用句柄、存在未关的 statement 或另一关闭尚未完成时，`close` 抛 `std.sqlite.Busy`，待操作结束可重试；其他关闭失败抛 `std.sqlite.CloseError`。一次关闭对全部别名生效，之后重复关闭幂等。已由 `next()` 返回的执行错误不会在 statement 释放后再被重复抛出。异步应用把整段查询与清理放进 `blocking`。

`withConnection(path) { db => ... }` 与 `withConnection(path, readOnly: true) { db => ... }` 在回调结束时关闭连接，包括返回 `Err` 或抛异常的路径。异步应用应把连接作用域和全部 SQL 操作放在同一个 `blocking` 块中，避免取消时把仍被原生工作使用的连接交给调用方。

## 写入与事务

`db.exec(sql)` 与 `tx.exec(sql)` 用于一次执行一条不带绑定参数、也不返回列的命令，例如 `CREATE TABLE`。`SELECT`、`RETURNING` 和参数化语句用 `prepare`。可用 `lastInsertRowid()` 读取连接最近一次成功插入的 SQLite rowid；即使这次插入随后回滚，SQLite 也不会重置该值。若要原子地关联插入和取 rowid，两步都在同一个事务回调里执行。

```lyra
import std.sqlite.{Connection}
import std.sqlite.{DbError}

fn addLog(db: Connection, payload: Bytes): Result<Int, DbError> {
    db.transaction<Int>() { tx =>
        val insert = tx.prepare("INSERT INTO logs(payload) VALUES(?1)")?
        defer insert.close()
        insert.bindBlob(1, payload)?
        insert.next()?
        tx.lastInsertRowid()
    }
}
```

`transaction` 接受一个同步回调。回调返回 `Ok` 时提交，返回 `Err` 或抛异常时回滚；若提交失败也先尝试回滚。退出前会关闭该事务所有仍未关闭的 statement。带出事务或 statement 的引用允许赋值和传参，但结束后再用会得到 `Closed`。

启动前须关闭该连接已有的 statement，且不能有手动开启的事务。事务期间使用回调传入的 `tx`：原 `Connection` 及其别名返回 `Busy`（`close()` 抛 `Busy`），同一连接不支持嵌套。`tx` 和它创建的 statement 在事务期间只供执行回调的线程使用，其他线程调用得到 `Busy`。事务体禁止手动执行事务或保存点控制语句；SQLite 因冲突自行回滚后，事务体不能继续向该连接写入。

回调的原始 `DbError` 或异常保持为主因。提交失败后回滚也失败时，返回值的 `DbError.cleanupError` 带附加错误；异常展开时可从 `Exception.cleanupFailures` 读取清理错误。回滚失败则关闭连接，避免后续操作误用未清理的事务。

异步调用把整个事务放进 `blocking`。取消会等待已经开始的事务安全收尾；回调仍可正常返回并提交。重试 SQLite 自身的锁冲突时，重新执行 `exec`、创建 statement 或重新开启整个事务；如果是句柄租用冲突返回的 `Busy`，原游标状态不变。
