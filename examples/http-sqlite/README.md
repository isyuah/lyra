# 有界并发 HTTP(S) 与 SQLite 服务

应用示例在 Windows 和 Linux 都原生编译，预编译 Lyra SDK 自带链接器、静态 SQLite、libcurl 和 OpenSSL。部署无需 Lyra、LLVM 或额外的 SQLite/TLS 动态库。服务默认绑定 `127.0.0.1`，启动后输出实际端口；可用 `--host` 和 `--port` 指定地址。

工作区包含两个包：根目录的 `example/http-sqlite` 负责监听、路由和状态码；`users/` 下的 `example/users` 负责 SQLite 查询与 JSON 数据类型。应用通过本地依赖调用 `findUser(path, id)` 获取 `Result<User?, DbError>`，并复用库公开的 `JsonCodec<T>` 工厂。

```sh
work=$(mktemp -d)
sqlite3 "$work/users.db" < examples/user-service/fixture.sql
lyra build examples/http-sqlite --locked --offline -o "$work/users-app"
"$work/users-app" "$work/users.db"
```

`GET /users/7` 返回 JSON，缺少用户为 404、错误 ID 为 400、数据库错误用 `std.log.Logger` 写 JSON 日志到标准错误并返回 500；日志记录路由模板、操作与错误码，不写 URL 查询值。其他已匹配路径的方法返回 405。`POST /users` 携带 `{"name":"Ada"}` 时，在独占事务内插入记录并读取 ID，提交后返回 201。`POST /teams/7?tag=a&tag=b&single=chosen` 保留宏路线，返回 `{"teamId":7,"tags":["a","b"],"label":"chosen","name":"Ada"}`。缺少或重复的单值 query、错误 JSON 和越界路径都在调用业务函数前返回 400；重复标签保留原顺序。

## HTTPS 与出站调用

```sh
"$work/users-app" "$work/users.db" --cert server.pem --key server.key \
  --upstream https://localhost:9443/users/7 --ca local-ca.pem
```

传入 PEM 证书链和私钥后启用入站 TLS 1.2/1.3，沿用同一 Router。`--upstream` 启用 `GET /upstream`，让服务请求固定上游 URL 并返回其状态和字节正文；不开启跳转。`--ca` 可指定出站专用信任根，省略时使用系统信任根，两种方式都校验证书和主机名。证书私钥不要提交或随应用日志输出。

## 路由与请求适配

手写 `GET /users/{id}` 处理器和宏生成的 `POST` 适配器共用 `Router` 注册表。`#post` 读取 `[path]`、`[query("tag")]` 和 `[body("newUserCodec")]` 参数标记，在同模块生成 `createUserRoute(router)` 挂载函数。body 解码使用从库导入的 `JsonCodec<NewUser>` 工厂；宏不扫描运行时类型。`@trace` 仍装饰业务函数，所以有效请求调用时会记录，参数校验失败和 JSON 解码失败不会调用它。

`Router.serve(listener, capacity)` 用固定数量的槽登记接收游标，达到容量时不会接受额外请求；每个请求收到完整消息后才分派给并发 handler。请求断开只取消所属 handler，原生响应和 SQLite 工作安全收尾后再关闭句柄。服务每个监听槽都在独立执行域推进，可在线程池只有一个阻塞 worker 时同时处理连接与查询。

## SQL 快照与取消

`#userQuery` 在 `users` 包内展开为普通类型检查的参数化 SQLite 调用。`users/lyra.json5` 只授予查询宏读取 `users/inputs/schema.sql` 和 `users/inputs/userById.snapshot`。修改查询时同步更新宏调用的 SQL 字面量和 `users/tools/userById.sql`，再显式运行 `python examples/http-sqlite/users/tools/prepareQuery.py` 刷新；可加 `--check` 只校验不写入。正常构建不执行准备脚本或联网更新。

[HTTP 验收](../../tests/http_cli.py)检查路径解码、重复 query、必填 body、错误状态、容量屏障与断线后游标释放；[SDK 验收](../../tests/installed_cli.py)从搬迁后的 SDK 原生编译 Debug/Release，使用临时 CA 验证入站 HTTPS、事务写入和出站调用。
