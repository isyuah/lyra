# 按 ID 查用户：一份 Lyra 小服务样例

先读 [src/main.ly](src/main.ly) 里的 `getUser(request: Request)`：`GET /users/7` 命中时回 JSON，查不到回 404，数据库报错回 500。代码里没有第四个“已取消”的 `match` 分支——客户端断开是请求停止执行，不是一种数据库查询结果。

这是设计样例，目前没有可运行它的 Lyra 编译器或库。`async fn`、冷 Task、`?`、块末值、`defer` 和 `blocking` 按各专题使用；异步入口见[编译器验收设计](../../rfc/compiler-acceptance.md)。HTTP 路由与具体 SQLite 方法名仍是示例 API。`#derive(Json)` 在编译期为 `User` 生成编解码实现。只删这行而保留 `User → Response` 转换，类型检查就会在转换体的 `Response.json(user)` 报缺少编码实现；反过来，只删转换声明，报错的是 handler 返回 `user` 的分支。

## 从请求到资源释放

- **HTTP 层**：`server.get(pattern, handler)` 传路径和函数值；匹配后才在当前请求作用域调用 `getUser`。`request.pathInt("id")` 非法或越界时回 400，不查库。成功分支 `Ok(Some(user)) => user` 需要 `Response`，因此使用 `User` 所在模块声明的[无名转换](../../rfc/0003-types.md)，生成 200 和 `application/json`。JSON 是这个转换体选的默认响应；404 和 500 仍由 handler 显式处理。
- **数据库层**：`findUser(id)` 创建一个冷 Task，`await` 才在当前请求作用域激活它。它的完成值是 `Result<User?, DbError>`：`None` 只表示查询正常结束但没有这行。`query.next()` 遇到 SQLite 的 `ROW` 得到 `Ok(true)`，`DONE` 得到 `Ok(false)`，出错得到 `Err`，不会误走 404。
- **阻塞边界**：`blocking` 接收同步的尾部 lambda，返回受当前请求管理的 Task。整个打开、prepare、绑定、step、读取、关闭都在隔离工作线程上完成；不让 SQLite 阻塞串行执行域。lambda 返回 `Result<User?, DbError>`，里面的 `?` 提前结束这次查询，不从外层 handler 非局部返回。参数 `?1` 绑定整数，不拼接 SQL。
- **资源边界**：每取得一个句柄就登记一次同步 `defer`。`query.text(1)` 把列值复制为 Lyra 的 `String`，带出工作线程的是 `User`，不是 SQLite 的行缓冲指针。块退出时先释放 statement，再关连接；`close()` 表示同步清理，清理异常按[错误与资源 RFC](../../rfc/errors-and-resources.md)处理。这里每请求打开一个只读连接，便于看清生命周期，不依赖连接池或 GC 何时运行。

## 一个请求里同时有 path、query 和 body

比如 `POST /teams/7/users?tag=a&tag=b`，JSON 请求体是 `{"name":"Ada"}`。注册仍然只传路径和函数：`server.post("/teams/{teamId}/users", createUser)`。请求来了才取值，注册时既没有请求体，也不需要罗列解码器。下面是普通函数和方法就能表达的**库 API 草案**：

```text
#derive(Json)
struct NewUser {
    pub name: String
}

async fn createUser(request: Request): Response {
    val teamId = request.path<Int>("teamId")
    if teamId == null { return Response.status(400) }

    val tags = request.queryAll("tag")
    match await request.json<NewUser>() {
        Ok(input) => await saveUser(teamId, tags, input),
        Err(_) => Response.status(400),
    }
}
```

这里 `path<Int>` 把命中的路径段解析成 `Int?`；`queryAll` 取同名键的所有字符串，按出现顺序得到 `["a", "b"]`，没传就是空列表。`request.json<NewUser>()` 在编译时要求 `NewUser` 有解码实现（此处由宏生成，手写也可以）；收到请求才异步读一次 body。若输入 `{}` 缺了必填的 `name`，解码返回 `Err`，由 handler 回 400。`saveUser` 是业务函数，成功时收到 `7`、两个标签组成的**一个列表**和**一个** `NewUser`。读取期间客户端断开走请求取消，而不是 JSON 解码的 `Err`。这些方法名可以更换，不需要为每种字段类型各发明一个关键字或 `pathInt` 式方法。

## 想连取参代码也省掉：一种可选短写

可以让业务函数直接声明每个参数来自哪里。以下是**替代上面的手写适配**的 HTTP 库提案：`#post` 是声明宏，`[path]`、`[query("tag")]`、`[body]` 是它读取的形参标记，用的是[已定的声明前缀](../../rfc/metaprogramming-data.md)。

```text
#post("/teams/{teamId}/users")
async fn createUser(
    [path] teamId: Int,
    [query("tag")] tags: List<String>,
    [body] input: NewUser
): Response {
    await saveUser(teamId, tags, input)
}
```

按[已定的宏规则](../../rfc/metaprogramming-data.md#f1-macro-inputs)，声明宏可以读取这些形参标记，并生成接收 `Request` 的同模块适配函数。适配器做与手写版相同的检查，成功才调用 `await createUser(teamId, tags, input)`；原函数仍能直接测试，若有 `@trace` 也照常生效。怎样显式挂载路由是 HTTP 库的 API 设计；不靠导入副作用，不把 `path`、`query`、`body` 做成语言关键字。需要流式 body 或自定义校验时仍可直接用 `Request`。

用同一请求检验短写：缺少必需路径段是路由未匹配；已匹配但整数越界、必填 body 缺失或 JSON 无法解码，回 400 且不调用业务函数。若再声明 `[query] notify: Bool = false`，仅缺失才用默认值；建议非法布尔值或同一标量传了两次都回 400，而 `tags` 明确是列表，允许重复。路径绑定名不在模板中，或声明了两个 `[body]`，建议在形参处报错，而不是等第一个请求才发现。

**函数的变长实参是另一回事。** 这里的业务函数有三个固定形参，不随 URL 中标签个数变化。普通 Lyra 函数可以把回调放在前面、变长形参放在最后；只有想把 lambda 写在括号外时，才要求最后一个形参是固定的函数参数。见 [RFC-0002 的调用规则](../../rfc/0002-syntax.md)。

## 四条验收路径

使用 [fixture.sql](fixture.sql) 在临时目录的一个新 `users.db` 文件中建表、插入 `7 / Ada`；样例服务以该目录为工作目录。每次查询会新开连接，所以不能把初始化连接独有的 `:memory:` 库当作共享夹具。下面是同一份源码的预期行为，默认清理本身成功。

| 场景 | 输入或安排 | 必须观察到的结果 |
|---|---|---|
| 命中 | `GET /users/7` | `Ok(Some(user))` → 200；JSON 解码后为 `{"id":7,"name":"Ada"}`，不比较键顺序。读取完的字符串在 statement 关闭后仍有效。 |
| 缺失 | 同一库，`GET /users/8` | `Ok(None)` → 404，空响应体；不是 DB 错误，不记错误日志。statement 与连接都关闭。 |
| 查询失败 | 换一个已存在、可只读打开但没有 `users` 表的临时库，仍请求 `/users/7` | 在 `prepare` 处得到 `Err(DbError)` → 500，空响应体。日志能关联此请求、看到 SQLite 报错和失败操作；此时只成功取得了连接，只清理连接。 |
| 请求取消 | 在查询尚未返回、响应尚未开始时，使服务确认客户端已断开 | 取消当前请求组，不走 `Err` 分支，不尝试发送 500 或其他响应。进行中的原生调用可以继续；它不再使用句柄后才清理，组退出时不能留下仍在执行的查询。 |

**取消怎样验收：** 测试绑定在 `next()` 已取得 statement、尚未返回时发信号并停在门闩。此时断开客户端，等请求组观察到取消；在不放行查询的情况下，同域另一个任务应能推进，且原生句柄仍有效、请求组尚未退出。然后放行，观察 statement 和连接分别释放一次，请求组才退出；全程不靠 `sleep` 猜时序，也不要求 C 线程反向调用 Lyra。这里取消的是首次激活查询的请求组，和旁观者放弃一次 `await` 不同。见[异步执行](../../rfc/async-execution.md)及[原生工具链的 SQLite 验收](../../rfc/native-toolchain.md)。

## 从写法得到的建议

**保留 handler 中的三分支。** 哪种结果是 404、哪种要记录并回 500，一眼就能看到，不必引入把 `None` 自动转成 HTTP 错误的语言规则。可顺便验一次诊断：删掉 `val result = await findUser(id)` 中的 `await`，前端应在 `match result` 附近指出拿到的是 `Task<Result<User?, DbError>>` 而不是 `Result`，提示等待这个任务。

**先缩短数据库封装，不加新语法。** 真正重复的是每条只读查询都要打开连接、prepare、bind、收一行并清理。值得试一个库方法：假设 `db` 是配置了路径的查询入口，`sql` 是源码中同一条参数化查询，整个 `findUser` 的函数体可以写成：

```text
await db.queryOptional(sql, [id]) {
    Ok(User(id: it.int(0)?, name: it.text(1)?))
}
```

这里有三个固定形参：SQL、绑定值列表、行解码函数。`[id]` 对应一个占位符，有多个就放在同一列表里，最后的块始终只处理行。这个方法只封装隔离执行和句柄清理：无行仍是 `Ok(None)`，列解码失败仍是 `Err`，请求取消仍须等原生操作安全收尾。
