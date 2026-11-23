# 可运行样例：异步 HTTP 查询 SQLite

在开发机从仓库根目录运行（`sqlite3` 命令只用来准备测试数据）：

```sh
work=$(mktemp -d)
sqlite3 "$work/users.db" < examples/user-service/fixture.sql
lyra build examples/http-sqlite -o "$work/users-app"
"$work/users-app" "$work/users.db"
```

第一行是本地端口。在另一个终端运行 `curl -i http://127.0.0.1:<端口>/users/7`，应得到 `200 application/json` 和 `{"id":7,"name":"Ada"}`。不存在的记录返回 `404`，非法路径参数返回 `400`，不支持的方法返回 `405`；数据库故障在标准错误记录操作和原因，客户端得到 `500`。按 Ctrl+C 结束前台服务。部署机只需要 Linux 系统库和 SQLite，不需要 Lyra、LLVM 或 `sqlite3` 命令。

## SQL 的准备、构建和执行

`#userQuery(db, "SELECT name FROM users WHERE id=?1", id)` 是本包用 Lyra 写的[表达式宏](src/.expressions/userQuery.ly)。工具链只读清单授予它的 `inputs/schema.sql`、`inputs/userById.snapshot`。校验通过后，生成对 `prepareUserById` 的普通调用：数据库连接和 `id: Int` 由编译器检查，执行时用 SQLite 的 `bindInt`，不拼接用户输入。

改动查询时，同时更新宏调用处字面量和 `tools/userById.sql`；改动 schema 时更新 `inputs/schema.sql`。显式运行 `python3 examples/http-sqlite/tools/prepareQuery.py`：用空的内存库装载 schema、编译 SQL 并核对参数和结果列；成功才更新快照。CI 可加 `--check` 只比对、不写入。普通 `lyra check/build` 不执行这个脚本，也不读取服务要使用的数据库。这是固定查询、显式 `Int → String` 契约的示例，不尝试从任意 SQL 表达式推断 Lyra 类型。

## 一个请求的生命期

1. `main` 用 `await blocking { server.accept(...) }` 接收请求，在自己的 `requestGroup` 中启动 handler。示例一次处理一条请求，让单线程阻塞池也能取得进展。
2. `handle` 在请求任务内登记 `watchDisconnect()`。查询工作在线程池中打开连接、绑定参数、复制结果并关闭 statement 和连接，只把 `Result<User?,String>` 传回执行域。
3. `route` 分别处理查到、未找到和错误三种结果。成功分支直接返回 `user`：该分支需要 `std.http.Response`，因此调用 `User` 所在模块的普通 `implicit` 转换；转换体用 `#derive(Json)` 生成的 `encodeUser` 编码。HTTP 层没有单独的 JSON 转义器。
4. 客户端断开时，请求任务被取消；正在运行的阻塞工作保留句柄直到安全返回。handler 注册的 `defer request.close()` 在子任务收尾后释放请求，取消不作为数据库错误，也不发送补救响应。`requestGroup` 等子任务结束再交还控制权，监听循环继续接受连接。

[端到端测试](../../tests/http_cli.py)在一个阻塞线程、真实 loopback 和临时 SQLite 下运行 Debug、Release；既测试大响应中途断线后同一实例还能处理请求，也覆盖中文、引号、换行和 NUL 的往返。

[取消夹具](../../tests/fixtures/http-cancel/src/main.ly)把 worker 停在已经读到第一行的游标上。请求组内有一个只等待该 worker 的观察任务；请求被取消时，它能先结束，让组外的观察者发出确认。此时另一个数据库连接无法取得独占锁。放行后，worker 读到第二行、释放游标和连接，请求组才退出；服务还能应答下一次 GET。整个验收不靠睡眠，不要求强行中止 `sqlite3_step`。更高层的路由、资源语法见 [RFC 服务示例](../user-service/README.md)。
