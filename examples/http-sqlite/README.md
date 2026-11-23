# 可运行样例：HTTP 查询 SQLite

准备一份 `users(id INTEGER PRIMARY KEY, name TEXT)` 的 SQLite 文件，然后运行 `lyra build examples/http-sqlite -o /tmp/users-app`、`/tmp/users-app /path/to/users.db`。第一行打印操作系统分配的本地端口。访问 `http://127.0.0.1:<端口>/users/7`；按 Ctrl+C 结束这个前台进程。

- 找到记录返回 `200 application/json`；名字中的双引号、换行、NUL 等会正确转义。找不到返回 `404`，无效的非负整数路径参数返回 `400`，数据库打开或查询失败返回 `500`。
- 路由、参数化 SQL 和业务状态由 Lyra 源码实现。`std.http` 是通用的监听、取 method/target/body 和发送响应 API，不内置用户表或路由。
- 当前示例按顺序处理连接，每次只处理一个请求并回送 `Connection: close`。它在查询前、响应前检查连接并及时关闭句柄；这一同步路径没有中断已经在执行的 SQLite 调用。异步工作与断线传递要单独验收。

`tests/http_cli.py` 用真实 loopback 和临时 SQLite 库验证 Debug、Release 的 HTTP 状态和内容；另一个带同步门闩的测试在请求已解析后明确断线，再检查后续连接仍能服务。此样例不同于 [RFC 中的最终服务写法](../user-service/README.md)。
