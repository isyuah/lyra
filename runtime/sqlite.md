# M6：系统 SQLite 的 C ABI 与源码绑定契约

[sqlite.h](sqlite.h) 是运行时内部的 SQLite 句柄接口，不要求编译器识别 SQLite 专用语法。[原生测试](../tests/runtime_sqlite.cpp)直接调用这层，覆盖真实 SQLite 内存库、确定性关闭与阻塞期间取消。公开的 [std.sqlite](../std/sqlite/README.md) 封装由[源码用例](../tests/sqlite_cli.py)和 [HTTP 服务用例](../tests/http_cli.py)另行验收。

## 调用约定

- 用 `lyraSqliteOpen(path, readOnly, &connection, &error)` 打开，用 `lyraSqlitePrepare(connection, sql, &statement, &error)` 准备**一条**语句。文本参数是 [Lyra String](strings.h) 指针，不是 C 字符串。文件名在边界复制成终止串；文件名和 SQL 中嵌 NUL 均报错，不截断。`readOnly != 0` 不会创建缺失库。
- `lyraSqliteBindInt`、`lyraSqliteBindText` 的位置从 **1** 开始。文本用 SQLite 自己的副本，不要求调用者一直保活输入。`lyraSqliteNext` 的 `ROW` / `DONE` 分别对应公开 API 的 `Ok(true)` / `Ok(false)`；失败是 `Err(DbError)`，和没有更多行不同。到 `DONE` 后重复调用保持 `DONE`，不重执行 SQL。
- `lyraSqliteReadInt`、`lyraSqliteReadText` 的列从 **0** 开始，必须已有行并且存储类型匹配。SQL `NULL` 单独返回 `LYRA_SQLITE_NULL`；空 `TEXT` 是 `OK` 和长度 0 的 String。SQLite 返回的 UTF-8 缓冲在 `step` / `finalize` 前按**字节长度**复制，故支持文本中嵌 NUL，关闭所有句柄后结果仍有效。
- `LyraSqliteError` 是调用方提供的输出位置；仅错误状态时读取。`operation` 是静态 C 名称，`message` 是已复制的 Lyra String，`code` 是 SQLite 错误码或封装所用的 SQLite 类别。公开绑定应按状态生成 `DbError` / 清理异常并保留这些信息，不能让 C 异常跨 ABI。参数指针和输出位置是 C 边界的调用契约，安全源码封装不暴露它们。

## 资源和取消

连接/statement 是 GC 托管的别名共享壳；C 句柄只在 `close` / `finalize` 里释放。先 `finalize` 后 `close`。已有活动 SQLite 调用或连接仍有 statement 时，原生 `close` 立即返回 `BUSY`，公开 `close()` 抛出 `Busy` 并保持连接打开。关闭成功后的后续操作返回 `DbErrorKind.Closed`，重复关闭幂等。SQLite 的 `finalize` 即使报告此前 `step` 的失败也已释放 statement；公开 API 不重复抛出已经由 `next()` 如实返回的失败。

异步调用方用 [阻塞入口](managed-tasks.md) 把整段 `open → prepare/bind → step/read → finalize → close` 放进已注册的工作线程。取消发出后可运行同域其他任务，但已提交工作及其句柄不能提前释放。验收用条件变量停在工作线程已经持有连接和 statement、尚未执行查询的位置：同域任务请求取消、尝试关连接并得到 `Busy`、触发 GC；放行后工作线程仍能读取、复制、关闭，任务才进入取消终态。没有异步中断 SQLite 或强杀线程的假设。

## 复验

Ubuntu 构建包：`libsqlite3-dev`、`libgc-dev`、`libuv1-dev`。SQLite 是系统动态库（`libsqlite3.so.0`），只有真正引用绑定的可执行文件才需要它。运行时构建关闭 C++ 异常；错误走 C ABI，原生程序不用链接 C++ 标准库。

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
