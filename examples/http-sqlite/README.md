# 有界并发 HTTP 与 SQLite 服务

应用示例在 Windows 和 Linux 都原生编译，预编译 Lyra SDK 自带链接器和静态 SQLite。部署时只需应用和数据库，无需 Lyra、LLVM 或额外安装 SQLite。服务启动后输出临时 loopback 端口。

工作区包含两个包：根目录的 `example/http-sqlite` 负责监听、路由和状态码；`users/` 下的 `example/users` 负责 SQLite 查询与 JSON 数据类型。应用通过本地依赖调用 `findUser(path, id)` 获取 `Result<User?, DbError>`，并复用库公开的 `JsonCodec<T>` 工厂。

```sh
work=$(mktemp -d)
sqlite3 "$work/users.db" < examples/user-service/fixture.sql
lyra build examples/http-sqlite --locked --offline -o "$work/users-app"
"$work/users-app" "$work/users.db"
```

`GET /users/7` 返回 JSON，缺少用户为 404、错误 ID 为 400、数据库错误记到标准错误并返回 500；其他已匹配路径的方法返回 405。`POST /teams/7?tag=a&tag=b&single=chosen` 携带 `{"name":"Ada"}` 返回 `{"teamId":7,"tags":["a","b"],"label":"chosen","name":"Ada"}`。缺少或重复的单值 query、错误 JSON 和越界路径都在调用业务函数前返回 400；重复标签保留原顺序。

## 路由与请求适配

手写 `GET /users/{id}` 处理器和宏生成的 `POST` 适配器共用 `Router` 注册表。`#post` 读取 `[path]`、`[query("tag")]` 和 `[body("newUserCodec")]` 参数标记，在同模块生成 `createUserRoute(router)` 挂载函数。body 解码使用从库导入的 `JsonCodec<NewUser>` 工厂；宏不扫描运行时类型。`@trace` 仍装饰业务函数，所以有效请求调用时会记录，参数校验失败和 JSON 解码失败不会调用它。

`Router.serve(listener, capacity)` 用固定数量的槽登记接收游标，达到容量时不会接受额外请求；每个请求收到完整消息后才分派给并发 handler。请求断开只取消所属 handler，原生响应和 SQLite 工作安全收尾后再关闭句柄。服务每个监听槽都在独立执行域推进，可在线程池只有一个阻塞 worker 时同时处理连接与查询。

## SQL 快照与取消

`#userQuery` 在 `users` 包内展开为普通类型检查的参数化 SQLite 调用。`users/lyra.json5` 只授予查询宏读取 `users/inputs/schema.sql` 和 `users/inputs/userById.snapshot`。修改查询时同步更新宏调用的 SQL 字面量和 `users/tools/userById.sql`，再显式运行 `python examples/http-sqlite/users/tools/prepareQuery.py` 刷新；可加 `--check` 只校验不写入。正常构建不执行准备脚本或联网更新。

[端到端验收](../../tests/http_cli.py)使用真实 loopback、SQLite 和独立临时目录运行 Debug/Release，检查路径解码、重复 query、必填 body、错误状态、容量屏障与断线后游标释放；另有独立的非 HTTP 应用验证库的查询和 JSON 接口。
