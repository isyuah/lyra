# Lyra RFC：装饰器、宏与数据交换

**状态：三类前缀、声明宏与表达式宏、宏的只读输入与权限边界、装饰顺序及 JSON/TOML 显式派生和字段映射已定。**  
**范围：** Attributes、受控 AST Macro、函数装饰器以及 JSON/TOML 结构化数据映射。

## 编译期常量

`const name: Type = expression` 在声明模块求值，直到换行或分号；`pub const` 可以像其他公开名称一样导入、换名和重导出。所有别名保持同一个常量身份，LLVM 将求得的值嵌入引用点，不生成运行时初始化器。`const fn` 在编译期可以调用，也可作为普通函数在运行时调用。

```lyra
pub const fn kibibytes(count: Int): Int { count * 1024 }
pub const chunkSize: Int = kibibytes(64)
```

`const` 声明要求标注结果类型，可以保存标量、文本和由它们组成的 Struct、Enum、Option、Result。每次引用按普通值语义使用，不运行原计算。`const fn` 要求显式结果类型，不支持入口函数或变长形参；普通同步函数无需添加 `const` 就可在编译期调用。所有被调用函数、用作函数值的函数和闭包都必须通过纯度检查，包括未执行的分支。I/O、异步、外部函数、用户隐式转换、变长调用、异常与清理及 `Any` / `dyn` 不在当前求值范围。

编译期函数可更新局部 `var` 和字段，创建并修改临时 `List<T>` 和普通 Class；Class 和列表通过别名共享状态，同步闭包按普通规则共享捕获的绑定。可在编译期间接收、传递、返回和调用纯同步函数值。List、Class 和函数值可以在同一次计算中使用，不能直接或嵌套保存到 `const` 结果。`for` 支持整数区间、列表、字符串、字节和能通过纯度检查的同步迭代协议；列表仍在进入时记录长度。泛型实例、默认实参、`this`、只读扩展属性、`match`、Option 收窄和提升按常规规则处理。编译期 `?` 可传播 Option，或传播错误类型相同且不需要转换的 Result。

求值遵守普通表达式顺序和短路。实际引用的常量形成依赖环时报 `LYCT0002`；整数溢出、除零和索引越界报 `LYCT0003`。每个求值根最多执行 100,000 步、递归调用深度 16、累计 32 MiB 常量数据（单个文本上限 1 MiB），保存的复合值最多嵌套 128 层；耗尽报 `LYCT0004`。这些错误定位到导致失败的引用或运算。

## 标记、装饰器与宏

Attribute 是附着于声明的结构化元数据，可以被工具、编译器和标准库读取；单独的元数据 Attribute 不改变程序直接调用函数的运行行为。

**声明宏和表达式宏都用 Lyra 编写，在编译期执行。** 宏接收调用处交给它的语法，生成已有 Lyra 语法能表达的代码；生成物与手写代码经过相同的名称解析、可见性、类型和泛型检查。运行已编译的业务程序不需要加载宏实现。宏不得添加词法符号或重新定义语法。

函数装饰器可包装函数调用。直接通过原名称调用，或取得该函数值再调用，都要经过装饰。首个验收例为不改变签名的 `@trace`：它装饰 `async fn` 后，调用仍得到冷 Task，等待两次不会执行两次函数体。首版通过受控 `syntaxAround(before, after, failure)` 生成包装函数；装饰器的具体能力见下文实现契约。

类型装饰器可读取 Struct/Class 的声明并生成同模块辅助声明，但不修改目标类型的字段、构造器或可见性。公开辅助 API 必须在 `@name(export helperName)` 中逐名授权，并像其他宏生成物一样通过普通解析与类型检查；类型装饰器不能返回 `syntaxAround`。这样可用于生成 builder、查询辅助函数或元数据入口，而不把运行时反射隐式塞进类型。

```text
@trace
async fn loadUser(id: Int): User { ... }

// 对 loadUser(id) 的直接调用也经过装饰行为
```

连续写 `@outer`、`@inner` 等价于 `outer(inner(fn))`；调用先进入 `outer`，再进入 `inner`，首版不允许改变公开参数或返回类型。

## 前缀与使用位置（已定）

| 能力 | 使用处写法 | 作用 |
|---|---|---|
| 标记（Attribute） | `[path]`、`[query("tag")]` | 把元数据附在声明上；标记本身不执行取参。 |
| 装饰器 | `@trace`、`@makeBuilder` | 函数装饰器包装同签名函数；类型装饰器可生成经显式授权的辅助声明。 |
| 声明宏 | `#derive(Json)` | 编译期读取类型声明，生成相应的派生实现。 |
| 表达式宏 | `val query = #query("SELECT 1")` | 在需要表达式的位置展开为表达式，其结果可赋值或传参。 |

前缀区分标记、装饰器和宏；`#` 在声明或表达式位置只接受相应种类的宏。`derive` 是声明宏，即使由标准库提供也使用 `#`。若错用装饰器前缀，应在 `derive` 处指出它是宏并提示 `#derive(Json)`，不自动改按宏展开。

下面用 HTTP 路由 API 展示三种前缀共存：

```text
#post("/teams/{teamId}/users")
@trace
async fn createUser([path] teamId: Int, [query("tag")] tags: List<String>, [body] input: NewUser): Response {
    await saveUser(teamId, tags, input)
}
```

声明宏可读取形参名 `tags`、显式类型 `List<String>` 和标记 `query("tag")`，并生成同模块适配函数。标准库 `#post` 据此生成 `request.queryAll("tag")`，再调用原 `createUser`，因此仍经过 `@trace`；生成的 `createUserRoute(router)` 与手写 handler 一样调用 `Router.post` 注册。运行时只解析本次请求的值，不需要反射查找形参或扫描路由。完整示例见[HTTP/SQLite 服务](../examples/http-sqlite/README.md)。

声明前缀位置的 `[path]` 是标记；表达式 `val ids = [id]` 中的 `[id]` 是列表，`users[id]` 是索引。局部声明可以附加哪些标记及其跨行规则仍开放。

<a id="f1-macro-inputs"></a>

## F1：宏展开与外部输入

**宏能看见什么。** 编译器提供带源码位置、尚未类型检查的语法节点。声明宏可遍历被标记的声明，包括签名、字段、形参标记与函数体，生成同模块的类型或函数；表达式宏读取自己的实参语法，生成替换调用位置的表达式。生成公开 API 须由源码显式请求。首版不向宏开放全程序类型检查后的 AST；可以让生成代码使用有类型要求的 API，再由普通类型检查器验证。

**宏能访问什么。** 项目显式按名称向指定宏提供只读输入，如查询快照、协议文件或模板。宏不能自行打开其他宿主文件，不能继承环境变量、联网或启动进程；传递依赖也不能自行扩大权限。调用语法、宏版本或所声明输入的内容变化后，不可复用旧的展开结果。错误应指回用户源码，而不是只显示生成文件的位置。

**外部工具何时运行。** 生成快照、调用外部生成器或向指定开发库做在线检查，都是用户显式发起并单独授权的[准备或检查步骤](modules-packages.md)。授权给具体工具，不传给宏。普通 `check/build` 的宏展开只消费已声明输入；缺少快照不代跑准备命令、不回退到连库。这里的离线是指宏展开；包解析的锁定和离线模式仍按包管理 RFC 执行。

### 首版宏接口（实现契约）

宏实现仍以 Lyra 函数表示，统一签名为 `fn name(target: String, argument: String): Syntax`。声明宏、表达式宏和函数装饰器分别位于包内 `src/.macros/`、`src/.expressions/` 和 `src/.decorators/`；同名实现由调用位置和 `#` / `@` 前缀区分。`Syntax` 入口避免拼接宏调用、实参插槽或装饰钩子时把源位置丢掉；`syntaxCode` 可用于有意构造普通代码文本，`syntaxArgument(i)` 则保留调用实参节点和作用域。生成物始终经常规 parser、名称解析和类型检查。

宏可遍历未类型检查的目标声明与函数体，也可以构造 Lyra 语法节点；它们看不到已完成全程序类型检查的 AST。`syntaxStart/End/Line/Column` 与生成片段的源映射令编译期错误回到手写调用或目标源码。默认宏只能生成私有辅助声明；生成公开 API 必须在调用处逐名授权，如 `#makeCodec(export publicCodec)`，且不能生成未授权的额外公开声明、`extern` 或公开重导出。节点数量、展开文本、求值步骤、调用深度和递归展开均受预算限制；预算耗尽时报告宏诊断。具体 Lyra helper、参数标记与 `#post` 示例见 [`std.meta`](../std/meta/README.md)。

命令行编译将完整的宏输出、源码映射与装饰器钩子缓存到工作区的 `.lyra/cache/compiler/macros/`；设置 `LYRA_CACHE` 后使用其下的 `compiler/macros/`。键包含编译器可执行文件指纹、目标平台、宏实现内容、调用所在源码及其映射，以及授予此宏的输入名称、路径与内容。命中只跳过宏程序的求值，生成代码仍按常规规则解析、检查授权并进行类型检查。缓存缺失、损坏或不可写时照常展开；不可读的未使用授权输入不影响展开。LSP 不读写持久宏展开缓存。

装饰器返回 `syntaxAround(before, after, failure)`，分别在原函数调用前、成功返回后或异常时插入受限语句；后置钩子读取 `result: Any`，异常钩子读取 `exception: Exception` 并由包装器原样重抛。首版不改变目标函数签名，async 包装仍产生冷 Task。它不需要运行时反射，也不让装饰器改写参数类型。

### SQL 示例：准备、编译、执行是三件事

`#query` 是库提供表达式宏的示例写法，`fetchOptional` 是普通库方法；SQL 不是 Lyra 关键字。

```text
val query = #query("SELECT name FROM users WHERE id = ?1", id)
val row = await query.fetchOptional(db)?
```

1. **准备**：开发者显式运行准备工具，让数据库检查字面量 SQL，保存每条查询的参数数量、可知的参数类型、结果列及可空性。可使用按应用迁移创建的临时库或指定的开发库。不执行业务查询，不保存行数据或凭据。若把 `name` 写成 `naem`，该步骤就能把缺列诊断映射回 SQL 字面量。
2. **编译**：普通构建仅向宏提供已声明的只读快照。匹配 SQL 文本、数据库方言、工具/格式版本和项目声明的 schema 或迁移摘要；缺少匹配项就指向 SQL 并提示刷新，不偷偷联网。宏生成参数绑定和行解码；已知需 `Int` 时传 `String`，由普通类型检查器指向实参。数据库（尤其 SQLite）不能确定类型或可空性时，要求显式标注或使用动态查询 API，不假装已经检查。CI 可另行用开发库核对快照；离线检查对快照负责，无法发现未记录的线上变更。
3. **运行**：这时才用本次的 `id` 向 `db` 执行参数化查询。无行仍为 `None`，数据库连接失败仍为 `Err`。编译通过检验的是查询与快照相符，不是数据库此刻可用。

参考：[Rust 过程宏](https://doc.rust-lang.org/reference/procedural-macros.html)的稳定接口消费并产生 token 流，不是整个程序的 Typed AST。它通常也有编译器进程的文件访问权，因此信任成本接近构建脚本；Lyra 可以借鉴其代码生成能力而选择不同的权限默认值。[SQLx `query!`](https://docs.rs/sqlx/latest/sqlx/macro.query.html) 可以在构建时使用开发库，也可用 [`cargo sqlx prepare` 生成的 `.sqlx` 元数据](https://github.com/transact-rs/sqlx/blob/main/sqlx-cli/README.md)离线构建。

## JSON/TOML 自动序列化

**JSON：显式派生，静态检查。** 要为一个普通业务 Struct 生成 JSON 编解码实现，必须在该类型声明前写 `#derive(Json)`。宏在编译期根据字段生成代码；程序在运行时用这些代码把值编码为 JSON，或把输入的 JSON 解码为该类型。仅声明 Struct、导入 JSON 库或调用 JSON API，都不会触发派生。若标准库或用户已经提供所需的编码或解码实现，直接使用即可，无需再写宏。

**显式运行时反射。** `std.reflect` 提供 `typeName(value: Any)`、`fieldCount(value: Any)`、`fieldNames(value: Any)` 和 `field(value: Any, name: String)`。运行时保留的类型 token 指向类型描述符；Struct/Class 描述符只列出公开字段，字段读取以原生字段类型重新装箱为 `Any`，仍可通过 `as? T` 做检查转换。私有字段不可枚举或读取。首版不支持动态写入、动态构造、枚举载荷反射或 Class 身份图遍历。该 API 是显式动态查询能力，不作为 JSON/TOML 静态 API 的回退；`#derive(Json)` / `#derive(Toml)` 仍在编译期生成静态编解码代码。

已有的 `JsonCodec<T>` 可通过 `jsonCodecList`、`jsonCodecOption`、`jsonCodecResult` 和 `jsonCodecStringMap` 组合。显式的 Option 和 Result codec 采用带 `tag` 的对象，带载荷的分支再加 `value`；多层 Option 因此可逆。组合保留嵌套路径和严格/宽松选项，不修改普通派生字段的 `null`/省略语义。

TOML 同样显式写 `#derive(Toml)`；已有实现直接使用，多项可合写为 `#derive(Json, Toml)`。两种格式只自动映射公开字段；未知输入键默认报错，诊断包含字段路径，接口可显式改为忽略。编码普通可空字段的 `None` 默认省略键；若省略会在解码时得到非空默认值，JSON 写 `null`，TOML 需显式表示或报错。

JSON 派生支持 `[jsonName("display-name")]`：编码和解码使用该键，源码中的字段名不变。别名不可为空或含 NUL；同一结构体有两个字段映射到同一键时编译报错。错误路径使用 JSON 键，而非源码中的字段名；非标识符键用方括号和 JSON 字符串表示，例如 `$.users[2]["display-name"]`。

JSON 解码每个对象时，先解码它的公开输入字段。缺少必填字段时报错，无默认值的可空字段缺失时取 `None`。其余缺省值按字段声明顺序求值，与普通构造共用声明处的默认表达式，可引用前面已确定的字段。私有字段必须有显式默认值，其类型无需提供 JSON 映射。编码时跳过私有字段，解码时将对应输入键按未知键处理。

显式 `null` 按字段类型解码。对 `String?` 这样的单层可空字段，未声明默认值或默认值直接写 `None` / `null` 时，编码 `None` 会省略键。使用其他默认表达式时一律写 `null`，这样即使表达式有副作用或每次返回不同的值，编码也无需执行它。

对 `Option<Option<String>>` 这样的双层可空对象字段，`{}`、`{"name":null}`、`{"name":"Ada"}` 分别映射到 `None`、`Some(None)`、`Some(Some("Ada"))`。派生支持省略默认值或直接以 `None` / `null` 为默认值。其他默认表达式可能占用缺键这一表示，需要手写映射。JSON 顶层或数组元素没有“缺失键”。TOML 没有 `null`，目标格式无法区分的状态须指定自定义表示或报错。

首个往返验收例（`META-005`）取只有公开 `id: Int32` 和 `name: String` 字段的 Struct，值为 `7` 和 `"Ada"`。JSON 用 `#derive(Json)` 生成实现：编码结果是对象 `{"id":7,"name":"Ada"}`，解码后两项字段值保持。TOML 按自身格式做相同的字段往返；不检查键顺序、空白或输出字节是否相同。Class 身份图和循环引用不在该验收范围内。

## 已接受验收行为

- 普通声明元数据本身不擅自改变函数执行。
- 函数装饰器改变直接调用可见的函数行为；装饰后仍满足原异步和类型契约。
- 类型装饰器只生成辅助声明，不改变被装饰类型本身；导出的生成 API 必须逐名授权并接受常规检查。
- `META-006`：在[小服务](../examples/user-service/src/main.ly)中保留 `User → Response` 的无名转换，只删掉 `User` 的 `#derive(Json)`，且不提供其他编码实现。`User` 仍可声明和构造；类型检查在转换体的 `Response.json(user)` 报缺少 `User` 的 JSON 编码实现。补回派生或提供手写编码实现即可通过。
- 若 `User` 请求了 JSON 派生，但字段 `address: Address` 的类型没有所需实现，编译期诊断指出 `address`，不会替 `Address` 派生。编解码实现齐备后，输入 JSON 缺少必填字段才是运行时的解码错误。
- `Option` 的多种空值状态在格式本身表达不了时，不得无声压扁或错误往返。
- 若 `name: String?` 默认 `"Ada"`，值为 `None` 时 JSON 写 `{"name":null}`，而不是解码后会成为 `"Ada"` 的 `{}`。默认严格模式对未知键 `profile.nmae` 给出完整路径。
- 宏生成的内部名字不意外捕获调用处同名变量。生成物不能绕过可见性、类型检查、泛型约束或源码诊断映射。

## 仍开放

- 属性与宏的展开阶段、多种 Attribute 混用时的先后顺序。
- 已完成类型检查后的只读类型查询。
- JSON/TOML 的时间、超大数字等特殊类型策略，以及自定义字段映射 API。
- 动态写入/构造以及枚举载荷反射的 API 与可见性规则。
- Class 与循环对象图是否支持序列化，以及如何避免身份和循环语义丢失。
