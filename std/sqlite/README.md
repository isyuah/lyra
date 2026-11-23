# std.sqlite：绑定参数查询

`std.sqlite` 用系统 SQLite 动态库，发布使用它的程序时目标 Linux 需要 `libsqlite3.so.0`。`Connection`、`Statement` 的底层句柄不公开。准备语句后用位置 **1** 起始的 `bindInt` / `bindText` 绑定值；读列的索引从 **0** 开始。`next().row()` 表示一行可读，`.done()` 表示结束；读取 SQL `NULL` 时 `.status.isNull()` 为真，和空串不同。

例如，读取已有 `notes.db` 中的一条留言：

```text
import std.sqlite.openReadOnly
import std.io.assert

fn main(): Unit {
    val db = openReadOnly("notes.db")
    assert(db.status().ok(), db.status().message())
    val query = db.prepare("SELECT body FROM notes WHERE id = ?1")
    assert(query.status().ok(), query.status().message())
    assert(query.bindInt(1, 7).ok(), "绑定失败")
    val step = query.next()
    if step.row() {
        val text = query.readText(0)
        if text.status.ok() { print(text.value) }
    }
    assert(query.close().ok(), "关闭查询失败")
    assert(db.close().ok(), "关闭数据库失败")
}
```

错误的 `.operation()`、`.code()`、`.message()` 可分别用于日志、分类和展示。`readText` 返回独立的 Lyra 字符串，关闭 statement 后仍可读取。未关闭的 statement 会让 `db.close().busy()` 为真；先关 statement 再关连接。这里的接口是同步的，不会将查询悄悄放入后台。
