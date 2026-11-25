# M6：SQLite 的 C ABI 与源码绑定契约

[sqlite.h](sqlite.h) 是运行时内部的 SQLite 句柄接口，不要求编译器识别 SQLite 专用语法。[原生测试](../tests/runtime_sqlite.cpp)直接调用这层，覆盖真实 SQLite 内存库、确定性关闭与阻塞期间取消。公开的 [std.sqlite](../std/sqlite/README.md) 封装由[源码用例](../tests/sqlite_cli.py)和 [HTTP 服务用例](../tests/http_cli.py)另行验收。

## 调用约定

- 用 `lyraSqliteOpen(path, readOnly, &connection, &error)` 打开，用 `lyraSqlitePrepare(connection, sql, &statement, &error)` 准备**一条**语句。文本参数是 [Lyra String](strings.h) 指针，不是 C 字符串。文件名在边界复制成终止串；文件名和 SQL 中嵌 NUL 均报错，不截断。`readOnly != 0` 不会创建缺失库。
- `lyraSqliteBindInt`、`lyraSqliteBindText`、`lyraSqliteBindFloat`、`lyraSqliteBindBlob` 和 `lyraSqliteBindNull` 的位置从 **1** 开始。文本和 BLOB 用 SQLite 自己的副本，不要求调用者一直保活输入。`lyraSqliteNext` 的 `ROW` / `DONE` 分别对应公开 API 的 `Ok(true)` / `Ok(false)`；失败是 `Err(DbError)`，和没有更多行不同。到 `DONE` 后重复调用保持 `DONE`，不重执行 SQL。
- `lyraSqliteReadInt`、`lyraSqliteReadText`、`lyraSqliteReadFloat` 和 `lyraSqliteReadBlob` 的列从 **0** 开始，必须已有行并且存储类型匹配。SQL `NULL` 单独返回 `LYRA_SQLITE_NULL`；空 `TEXT` / `BLOB` 是 `OK` 和长度 0 的 String / Bytes。SQLite 返回的缓冲在 `step` / `finalize` 前按**字节长度**复制，支持文本中嵌 NUL，关闭所有句柄后结果仍有效。
- `LyraSqliteError` 是调用方提供的输出位置；仅错误状态时读取。`operation` 是静态 C 名称，`message` 是已复制的 Lyra String，`code` 是 SQLite 错误码或封装所用的 SQLite 类别。公开绑定应按状态生成 `DbError` / 清理异常并保留这些信息，不能让 C 异常跨 ABI。参数指针和输出位置是 C 边界的调用契约，安全源码封装不暴露它们。

## 资源和取消

连接/statement 是 GC 托管的别名共享壳。先 `finalize` 后 `close`。已有活动 SQLite 调用或连接仍有 statement 时，原生 `close` 立即返回 `BUSY`，公开 `close()` 抛出 `Busy` 并保持连接打开。关闭成功后的后续操作返回 `DbErrorKind.Closed`，重复关闭幂等。SQLite 的 `finalize` 可能重复报告此前 `step` 的失败；绑定会释放句柄、保留原 `next` 的失败，不再重复抛出。

`lyraSqliteExec` 要求一条不带参数、不产生结果的语句，校验通过后才推进一次；临时 statement 无论成功失败均释放。`lyraSqliteBegin` 获取并持有连接的独占访问权，直到同一原生线程调用 `lyraSqliteFinish`。事务内创建的 statement 在其作用域中登记和解登记；finish 先关闭剩余 statement，再提交或回滚。外部访问返回 `BUSY`；作用域结束后，引用存续但原生访问返回 `CLOSED`。SQLite 自动回滚后，仍要调用 finish 释放作用域，作用域内不能再执行其他语句。若请求提交却失败，finish 在释放连接前尝试回滚；两者均失败时，调用方可从附加 `LyraSqliteIssue` 取得回滚错误，连接关闭。

异步调用方用 [阻塞入口](managed-tasks.md) 把整段 `open → prepare/bind → step/read → finalize → close` 放进已注册的工作线程。取消发出后可运行同域其他任务，但已提交工作及其句柄不能提前释放。验收用条件变量停在工作线程已经持有连接和 statement、尚未执行查询的位置：同域任务请求取消、尝试关连接并得到 `Busy`、触发 GC；放行后工作线程仍能读取、复制、关闭，任务才进入取消终态。没有异步中断 SQLite 或强杀线程的假设。

## 复验

从源码构建时，Ubuntu 需要 `libsqlite3-dev`、`libgc-dev`、`libuv1-dev`。SDK 仅在应用引用 SQLite 绑定时把它静态链接进产物。运行时构建关闭 C++ 异常；错误走 C ABI。

```bash
out=$(mktemp -d)
for opt in 0 2; do
    clang-18 -std=c++23 -O"$opt" -fno-exceptions -fno-rtti -I runtime -I include \
      runtime/{managed,strings,tasks,context,exceptions,sqlite}.cpp tests/runtime_sqlite.cpp \
      $(pkg-config --cflags --libs bdw-gc libuv sqlite3) -o "$out/query"
    "$out/query" && UV_THREADPOOL_SIZE=1 "$out/query" || exit 1
done
readelf -d "$out/query" | grep NEEDED
```
