# RFC-0003：类型系统、泛型与 Trait

**状态：无名隐式转换、跨模块可见性、泛型实参推断及首版重载排序已接受。** 返回值的[隐式转换](language-core.md)及[泛型结构约束](generics-and-interfaces.md)见各自专题。[`?` 传播不同具体错误](errors-and-resources.md#c1-conversions)时复用同一 `implicit` 声明与查找规则。

## 声明 `User → Response`，不用给转换起名

[小服务](../examples/user-service/src/main.ly)在定义 `User` 的模块顶层声明默认响应：

```text
implicit (user: User): Response { Response.json(user) }
```

转换的签名是有方向的类型对 `User → Response`。`user` 只是实现体里的形参，改名不影响查找。handler 的返回类型是 `Response`，所以可以直接 `return user`。本例中 `User` 已能编码为 JSON，是转换体选用 `Response.json(user)` 让默认响应成为 JSON；某个接口要回 HTML，就显式写 `return Response.html(user)`。

- **在哪里声明：** 源类型或目标类型的定义模块都可以声明，不必写进类型体。同一模块对同一方向的类型对只能声明一次，即使分在不同文件。类型别名和 re-export 不改变类型身份，也不让第三个模块变成类型的定义方。
- **在哪里生效：** 默认只在声明模块内可用；两端类型公开时，写 `pub implicit (user: User): Response { Response.json(user) }` 可供其他模块使用。编译器按实参或返回值的静态类型、所需的目标类型，到两端的定义模块查找可见声明。调用者无需单独导入转换；即使类型由上下文推断而来，也按同一类型对查找。
- **遇到冲突：** 两端属于不同模块且各提供一条可见的同向转换时，只有在确实需要转换的使用处才报歧义。诊断列出类型对和两处声明位置，不靠模块或导入顺序选一个。

## 用 `send(user)` 看重载选择

```text
fn send(value: User): String { "user" }
fn send(value: Response): String { "response" }
```

**已定：首版允许同名函数按形参类型重载。** `user: User` 精确匹配 `send(User)`；若只有 `send(Response)`，则需要一条可见的直接 `User → Response` 转换。每个实参最多使用一条用户转换。若选中的签名需要转换，但该类型对有两条同等适用的声明，在实参处报歧义并列出两处声明。选择重载时不执行转换体；选定后才在运行时对需要转换的实参各执行一次。

| 验收编号 | `send(user)` 的条件 | 必须观察到的结果 |
|---|---|---|
| T3-001 | 保留上例两个重载 | 返回 `"user"`，不执行转换；即使两端各有一条可见转换，精确匹配仍胜出。 |
| T3-002 | 只保留 `send(Response)`，有一条可见的 `User → Response` 转换 | 执行一次转换体，然后返回 `"response"`。 |
| T3-003 | 只有 `send(Response)`；`User` 与 `Response` 的定义模块各公开一条 `User → Response` 转换 | 在实参 `user` 处报歧义，列出两处声明位置。改为 `send(Response.json(user))` 或 `send(Response.html(user))` 则精确匹配，不执行隐式转换。 |
| T3-004 | 只有 `send(Response)`，只有 `User → Profile` 和 `Profile → Response` 两条转换 | 在 `user` 处报需要 `Response`，不自动串接两次转换。 |

## 跨模块验收

测试用同一个包的三个模块：`model` 定义公开的 `User`，`web` 定义公开的 `Response`，`app` 消费二者。各用例独立，按[模块可见性](modules-packages.md)检查，不要求 HTTP 标准库依赖业务代码。

| 验收编号 | 安排 | 必须观察到的结果 |
|---|---|---|
| T3-005 | 只有 `model` 声明 `pub implicit (user: User): Response`；另测只在 `web` 声明 | 两种情况下，`app` 只导入类型就能在返回 `Response` 的函数里写 `return user`。换用 `User` 的公开别名，结果不变。 |
| T3-006 | 在 `model` 声明同一转换但不加 `pub` | `model` 内仍可用；`app` 中的 `return user` 在 `user` 处报需要 `Response`，不是报缺少某个导入。 |
| T3-007 | `app` 导入 `User` 或用 `pub import` 给它起公开别名后，自己声明 `User → Response` | 在转换声明处报错：`app` 没有定义这两个类型中的任何一个。普通显式适配函数仍可用。 |
| T3-008 | 在 `model` 的两个文件里分别声明 `User → Response`，只改形参名 | 报重复声明并指出两处位置；不等有调用才报歧义。 |

## 固定参数重载：逐个实参比较，不算总分

```text
fn send(first: User, second: Response): String { "userFirst" }
fn send(first: Response, second: User): String { "userSecond" }
```

**已定。** 泛型候选的类型实参已确定且满足约束时，把类型实参代入形参签名，与非泛型候选一起比较。不满足约束或有实参不能匹配的候选先排除。

1. 对每个实参，**精确匹配优于内置无损数值提升，内置无损数值提升优于直接的用户转换**。
2. 一个候选在每个位置都不差于另一个、且至少一处更好，才按匹配质量胜过后者。比较所有适用候选后，若剩下的候选在不同位置各有优势，就在**整次调用**处报重载歧义，指出各签名需要转换的实参位置。不按转换总次数、参数先后、声明或导入顺序挑选。
3. 非泛型与泛型候选都逐项精确匹配时，选非泛型。若非泛型需要转换而泛型逐项精确，则选泛型。

设 `user: User`，只存在 `User → Response` 这一个方向的一条可见转换；显式工厂 `Response.json(user)` 返回 `Response`。

| 验收编号 | 调用 | 必须观察到的结果 |
|---|---|---|
| T3-009 | `send(user, user)` | 编译失败。诊断列出两个签名：`send(User, Response)` 需转换第 2 个实参，`send(Response, User)` 需转换第 1 个。交换声明顺序仍报同一歧义。 |
| T3-010 | `send(user, Response.json(user))` | 返回 `"userFirst"`；两个实参都精确匹配，不再插入隐式转换。 |
| T3-011 | `send(Response.json(user), user)` | 返回 `"userSecond"`；同样不插入隐式转换。 |
| T3-012 | 另加返回 `"bothUsers"` 的 `send(User, User)`，再调用 `send(user, user)` | 返回 `"bothUsers"`；有全部精确匹配的候选时，不因前两个候选彼此打平就提前报错。 |
| T3-013 | `send(user, user, user)`，只有 `send(User, Response, Response)` 和 `send(Response, User, User)` | 在调用处报歧义：前者需转换第 2、3 个实参，后者需转换第 1 个。即使后者只需一次转换也不胜出。 |

## 无损提升与泛型重载的验收

每行独立。`count: Int32` 和 `user: User` 是类型已知的变量；`Int32 → Count`、`User → Response` 各有一条可见的直接用户转换。

| 验收编号 | 调用与候选 | 必须观察到的结果 |
|---|---|---|
| T3-014 | `write(count)`，候选为 `write(Int64)`、`write(Count)`；另测增加 `write(Int32)` | 前者选 `write(Int64)` 并做内置提升；增加精确匹配后选 `write(Int32)`。两次都不执行 `Int32 → Count` 的转换体。 |
| T3-015 | `send(user)`，候选为 `send(User)` 和 `send<T>(T)`，其中 `T = User` | 两者都精确，选非泛型 `send(User)`。 |
| T3-016 | `send(user)`，候选为 `send(Response)` 和 `send<T>(T)`，其中 `T = User` | 选精确匹配的泛型，不执行 `User → Response` 转换体。 |

## 泛型调用：在传参时转换，还是在返回时转换？

**已定。** 省略类型实参时，如果实参的静态类型已直接确定 `T` 且满足约束，外层函数的返回类型不会借用户转换改选这个 `T`。先以推得的类型调用泛型函数；若结果需要转换才符合外层声明的返回类型，就在返回处转换。显式指定类型实参时，按指定后的形参类型检查实参，需要的转换在进入函数体前执行。这不影响 [RFC-0002](0002-syntax.md) 中 `val name: String? = absent()` 从目标类型补出 `T` 的规则。

以下是独立验收样例：`identity` 没有其他重载，`User` 的定义模块只声明这一条转换。`note` 每次向记录追加一个标签，各用例从空记录开始。

```text
fn identity<T>(value: T): T { note("identity"); value }
implicit (user: User): Response { note("convert"); Response.json(user) }

fn reply(user: User): Response { identity(user) }
fn replyExplicit(user: User): Response { identity<Response>(user) }
```

| 验收编号 | 输入 | 必须观察到的结果 |
|---|---|---|
| T3-017 | `reply(user)` | `identity` 收到 `User`，`reply` 把调用结果转为 `Response`；记录恰为 `identity, convert`。 |
| T3-018 | `replyExplicit(user)` | `user` 先转为 `Response` 再传给 `identity`；返回处无需再转，记录恰为 `convert, identity`。 |
| T3-019 | 删除转换声明，两个入口分开编译 | `reply` 在块末的 `identity(user)` 报需要 `Response`、得到 `User`；`replyExplicit` 在 `identity<Response>(user)` 的实参 `user` 处报同一类型不匹配。两处的 `T` 都已确定。 |

## 多实参推断与重载平局（B1、B2）

对同一 `T`，静态类型相同就取该类型；不同整数类型取能无损容纳它们完整取值范围的**唯一最窄**共同整数类型。不存在唯一结果则要求显式给出 `T`，不猜 `Any`，也不搜索用户转换来统一它们。显式指定后照常检查每个实参。

对同一个实参，精确匹配优于 `T → T?`，后者优于用户转换；可空提升与无损数值提升之间不硬排。同符号整数有多个无损提升时取最窄目标。无后缀整数字面量优先 `Int`；没有该候选时，只在唯一目标容得下时选它。仍按实参逐项比较，不用后续规则打破不同位置各有优劣的歧义。匹配质量相同，先沿用全精确时的非泛型优先，再选固定而非变长签名，再选用默认值较少者；多个候选仍并列则列出签名报错。

实例与静态方法分别应用这套规则：`value.read(...)` 和 `Type.read(...)` 各自只在可见候选中选择，接收者不参与实参排序，也只求值一次。类型声明上的形参不算方法自己的泛型形参。通过结构约束调用的方法按约束声明的完整签名分派，不会在具体实例化时改选接收者上的其他重载。构造器 `init` 不重载。

| 验收编号 | 输入 | 必须观察到的结果 |
|---|---|---|
| T3-020 | `choose<T>(left: T, right: T)` 收到 `Int32` 和 `Int64` 变量；另测 `Int64` 和 `UInt64` | 前者取 `Int64`；后者没有足够宽的首版整数类型，在调用处要求明确类型或转换。 |
| T3-021 | 同一 `choose` 收到 `User` 和 `Response`，仅有 `User → Response` 转换 | 省略类型实参不能猜 `T`；`choose<Response>(user, response)` 则只转换 `user`。 |
| T3-022 | `count: Int32`，`write` 只重载 `Int32?` 和 `Int64` | 可空和数值提升打平，在调用处列出这两个候选；增加 `write(Int32)` 则选精确匹配。 |
| T3-023 | `f(Int)` 与 `f(Int...)` 接收 `1`；另测仅有 `f(Int8)` 和 `f(Int16)` 接收 `200` | 前者用固定签名；后者只有 `Int16` 能容纳该字面量。换为 `1` 则歧义。 |
| T3-024 | `f(Int, Int = 0)` 与 `f(Int, Int = 0, Int = 0)` 接收 `1`（签名简写） | 匹配质量相同，选只用一个默认值的候选，不执行另一候选的默认表达式。 |
