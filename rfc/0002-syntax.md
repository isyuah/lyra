# RFC-0002：词法、表达式、函数与控制流

**状态：首版调用、换行、运算符分组、求值顺序、lambda 和基本控制流已定。** 依据[语言核心](language-core.md)和[集中确认单 A1、A2、D2](../design/06-v1-batch-review.md)。[正反例](0002-syntax-cases.md)给出预期结果和报错位置。本篇不定义 HTTP API。

## 1. 调用：先看签名，不看实参长相

**已定。** 函数是一等值，可放在任何与其类型相符的形参位置。`server.get("/users/{id}", getUser)` 是两个固定实参：一个路径字符串，一个函数值。注册时不调用 `getUser`。请求到达时，库才把一个 `Request` 交给它；若 `getUser` 声明为 `async fn ...: Response`，库等待的是 `Task<Response>`。这不限制一个请求能带多少字段，也不需要语言猜“最后传进来的是什么”。

### 固定、默认和变长参数

- 位置实参在命名实参之前，依次占用固定形参；不因为下一形参有默认值或类型不合就自动跳过它。命名实参可以换位；同一固定形参最多绑定一次。
- 显式实参按调用处从左到右各求值一次，再按名称/位置绑定。只对缺省项求默认值，按形参声明顺序；可引用更早的形参，实例方法的默认值还可访问 `this`。调用 `async fn` 时这两步都在创建 Task 时完成，不推迟到首次 `await`。

**已定：变长形参写 `name: T...`。** `...` 放在元素类型之后。`values: Int...` 接收零个或多个 `Int`，而 `values: List<Int>` 是接收一个列表的固定形参。每个函数最多有一个变长形参，必须放在最后；它只收位置实参，逐个按元素类型检查，不能按名字追加。

**已定：调用者决定是否展开，首版只允许展开列表。** `...ids` 的操作数须具有 `List<T>` 静态类型。列表表达式只求值一次，在变长段的当前位置按顺序传入各元素；空列表贡献零个实参。迭代器即使能产出 `T`，也不能直接展开；需要时先显式收集成列表。写 `ids` 时，列表本身就是一个实参，按其类型正常检查。展开只填变长段，不替前面的固定形参补值。

例如 `ids: List<Int>` 的值为 `[2, 5]`，`countArgs(values: Any...)` 返回收到的实参数量。`countArgs(ids)` 得到 `1`，`countArgs(...ids)` 得到 `2`。若变长段要求 `Int`，`visit(printInt, ids)` 就在 `ids` 处报类型不匹配，`visit(printInt, ...ids)` 则依次传入 `2`、`5`。

```text
fn visit(onValue: (Int) -> Unit, values: Int...): Unit {
    for value in values { onValue(value) }
}

visit(printInt)                  // 零个数，不调用 printInt
visit(printInt, 2, 5)            // 回调在第一个位置；依次打印 2、5
visit(printInt, ...ids)          // ids 是 List<Int>
```

**已定：声明 `values: T...` 时，函数体里的 `values` 是本次调用独立的普通可变 `List<T>`。** 按调用处的顺序收集变长段的元素；没有元素时得到空列表。`values` 可以直接传给接收 `List<T>` 的函数，也可以作为 `List<T>` 返回。

独立的是列表，不是元素指向的对象。用 `...ids` 展开 `[2, 5]` 后，写 `values[0] = 9` 只改本次调用的列表，`ids[0]` 仍是 `2`。元素遵循普通的[值与引用语义](language-core.md)：若元素是 Class 引用，改 `values[0].name` 会改到同一对象，换掉 `values[0]` 则不会换掉调用者列表的第一项。要原地修改调用者的列表，应声明固定形参 `values: List<T>` 并直接传 `ids`。这些规则约定可观察行为，不要求每次调用都实际分配。

### 尾部 lambda 是可选写法

**已定。** 只有声明的**最后一个形参本身是固定的函数参数**，才允许把它的 lambda 写在调用括号外。不从变长实参末尾挑一个函数当回调，也不把函数类型的变长形参当作固定槽位。若希望 API 支持尾部 lambda，可令前面的固定形参接收列表：

```text
fn visitList(values: List<Int>, onValue: (Int) -> Unit): Unit {
    for value in values { onValue(value) }
}

visitList(ids, printInt)
visitList(ids) { printInt(it) }
```

**已定。** 函数类型写 `(Int) -> Unit`，显式 lambda 写 `(value: Int) => { printInt(value) }`，可标返回类型，如 `(value: Int): Int => { value + 1 }`。上例第二行调用等价于 `visitList(ids, (value: Int) => { printInt(value) })`。尾部块直接绑定最后一个形参，因此 `visitList(values: ids) { printInt(it) }` 不算命名实参后又跟了位置实参；该槽位已显式绑定时在 `{` 报重复。没有其他实参时可写 `blocking { ... }`。

lambda 捕获绑定本身。期望恰好一个参数时可省略名字使用 `it`；嵌套时指最内层隐式参数，要同时用外层参数就显式命名。`it` 不是隐式接收者。`return` 和传播失败的 `?` 只退出最近的函数或 lambda：`blocking { ... }` 中的 `?` 结束这次查询，不从外层 HTTP handler 返回。

### 泛型函数是首版能力

**已定。** 普通函数可以引入类型参数，有约束时用 `T: Render`，按可见成员的结构签名检查，不要求类型预先声明实现。它和路由、宏、是否异步无关。

```text
fn identity<T>(value: T): T { value }
fn absent<T>(): T? { null }

val count = identity(7)
val label = identity<String>("Ada")
val name: String? = absent()
```

**已定：省略类型实参时，实参类型和明确的预期结果类型都可用于推断。** `identity(7)` 从实参推得 `T = Int`；`identity<String>("Ada")` 则显式指定 `T = String`。`absent()` 没有实参，但 `name: String?` 给出了目标类型：把声明的返回类型 `T?` 与 `String?` 对应，就得到 `T = String`，调用结果是 `String?` 的 `None`。

若仍有类型参数缺少推断线索，就在调用处指出是哪一个，不默认取 `Any`。例如 `val value = absent()` 应在 `absent()` 处提示无法推断 `T`，可写 `absent<String>()` 或给 `value` 补 `: String?`。推断结果仍须满足函数声明的[泛型约束](generics-and-interfaces.md)。若实参已直接确定 `T`，外层返回位置需要的用户转换作用于调用结果，不改变这个 `T`；与显式指定 `T` 的对照见 [RFC-0003](0003-types.md)。

**已定：显式泛型调用按语法识别。** 表达式中，函数或成员名紧接 `<`、其后在语法上构成完整的类型实参列表、闭合 `>` 紧接 `(` 时，按泛型调用解析，如 `identity<Int>(7)`、`request.json<List<User>>()`。类型位置允许嵌套闭合 `>>`。`left < right` 是比较；`left<right>(7)` 是泛型调用。若 `right` 只是值，就在 `right` 处报需要类型，不回退成比较，也不靠首字母大小写区分。显式类型实参配尾部块写 `work<Int>() { ... }`。

类型名后的 `<...>.` 用于限定静态调用，例如 `Box<Int>.make(7)` 或 `Box<Int>.from<String>("7") { it.size }`。类型实参省略时，`Box.make(7)` 中的 `Box` 先按值名称解析；若没有同名参数、局部绑定或实例字段，再尝试按类型限定调用。详见[实例方法与静态方法](language-core.md#实例方法与静态方法)。

## 2. 标识符与字面量

- **已定：** 在声明前缀位置，`[path]` 是标记，`@trace` 是函数装饰器，`#derive(Json)` 是声明宏调用；在表达式位置，`#query(...)` 是表达式宏调用，`items[index]` 仍是索引。宏种类必须与使用位置匹配。见[元编程与数据交换](metaprogramming-data.md#f1-macro-inputs)。
- **已定：** 标识符区分大小写，首版为 ASCII 字母或 `_` 开头，后接 ASCII 字母、数字或 `_`，关键字作名字用反引号转义。Lyra 自有 API 和示例采用驼峰：函数、方法、参数、局部值、字段用 `lowerCamelCase`（`getUser`、`teamId`），类型、trait 和枚举分支用 `UpperCamelCase`（`Request`、`Some`）。这是命名风格，不是让词法器拒绝下划线；C ABI 原符号如 `sqlite3_open` 不改名，SQL 和 JSON 的外部键名也不受影响。
- 当前保留词为 `fn val var const if else while for in return break continue true false struct class enum trait pub import as is async await spawn extern match null implicit defer throw try catch this static`。`type` 在顶层引入透明类型别名，在 trait 中引入关联类型；`init` 仅在对象声明体内引入构造器；`blocking`、`taskGroup` 和 `parallel` 仅在表达式中紧接同一行块时作为任务操作；`unsafe` 同样只在表达式中紧接同一行块时引入[原生内存操作作用域](native-toolchain.md#原生指针与闭包回调)；`extension` 只在声明起始处引入扩展组；`it` 只在单参尾部 lambda 内自动绑定。裸表达式 `None` 表示空值，同名绑定可用反引号引用；在模式中按被匹配的类型解析，反引号可以强制把裸名称当绑定。
- **已定：** `//` 行注释及 `/* ... */` 块注释。块注释不嵌套，遇到首个 `*/` 结束；到文件末尾未闭合，定位开头的 `/*`。注释不拼接两侧 token，内部的换行参与和普通空白相同的换行规则。
- **已定：** `Int`/`UInt` 为 64 位；支持十进制、`0x`、`0b`、`0o` 和数字分隔符 `_`。无后缀浮点默认 `Float64`。双引号为字符串，单引号为单个 Unicode 标量。`${expr}` 在匹配的 `}` 结束；内部的字符串和花括号按表达式解析，而不是遇到第一个 `}` 就截断。插值按 `Display` 的 `toString` 检查，失败定位到插值内的表达式。
- 字符和普通字符串支持 `\n`、`\r`、`\t`、`\0`、`\\`、转义引号和 `\u{1F642}`（1–6 个十六进制数字，必须是 Unicode 标量）；字符串中可写 `\${` 输出字面量 `${`。只有插值表达式和原始字面量可跨行。
- `r"..."` 或 `r#"..."#` 是原始字符串。可在引号两侧增添相同数量的 `#` 以容纳内部引号，不转义、不插值，保留源码中的实际换行字节。`b"\xFF"` 和原始字节串 `br#"..."#` 产生不可变 `Bytes`。字节串接受 `\xHH`，但不做 Unicode 转义；源码中直接写入的文字仍使用它的 UTF-8 字节。
- 插值从左到右逐个求值，数值、`Bool`、`Char`、`String` 直接支持。其他类型提供可见的同步实例方法 `fn toString(): String` 即可；泛型体内要通过结构约束声明该签名。不会尝试任意隐式转换或反射。

## 3. 换行、分号与块

**已定。** 分号可省。块正常执行到 `}` 时，`{ expr }` 取 `expr` 的值；`{ expr; }` 仍执行 `expr`，但丢弃结果，块值为 `Unit`。空块也是 `Unit`；末尾表达式与 `}` 之间的换行不丢弃值。函数体、lambda 和 `if` / `match` 的分支块遵守同一规则。

分号不改变声明的返回类型：`fn total(): Int { 1 + 2; }` 在 `;` 处报需要 `Int`、得到 `Unit`，去掉分号即可。显式 `return` 不依赖块末取值：`fn total(): Int { return 1 + 2; }` 返回 `3`。

**已定：** LF 和 CRLF 同义。当前表达式未完成（如行末 `=`、二元运算符或 `.`）则继续读；圆括号和方括号内的换行是空白，不代替逗号。嵌入其中的 `{ ... }` 内仍按块分隔语句。能结束一个语句时换行分隔，另留三条明确边界：

- 行首 `.` / `?.` 可继续上一行的接收者，方便方法链。其他行首二元运算符不回接已完整的表达式。`price +` 换行 `tax` 可以相加；`price` 换行 `+ tax` 不相加（本草案不设一元 `+`）。
- `return` 后直接换行是无值返回，不把下一行变成返回值；`else` 可另起一行接前一 `if`。尾部 lambda 的 `{` 必须与调用结尾同一行，不能把下一行独立的块吸作实参。
- `if` / `while` 的条件、`match` 的被匹配值和 `for ... in` 的集合，在最外层遇 `{` 开始**控制流的块**，不尝试尾部 lambda。`if ready() { start() }` 没有给 `ready` 加回调；条件确实含尾部 lambda 时写 `if (items.any { it.active }) { start() }`。括号内恢复普通调用规则。

## 4. 运算符怎样分组

**已定：`await` 比后缀 `?` 结合得更紧。** 若 `findUser(id)` 得到 `Task<Result<User?, DbError>>`，`await findUser(id)?` 就是 `(await findUser(id))?`：先等待，再检查 `Result`。在返回兼容 `Result` 的异步函数里，`Err` 才提前返回；`Ok(None)` 得到 `None`，继续执行。显式写 `await (findUser(id)?)` 则在 `?` 报错，因为它作用在 Task 上。

**首版优先级（已定）。** 下表从紧到松；同级中缀默认左结合，例外在行末注明。重载不能改变语法树。

1. 成员 `.`、安全访问 `?.`、索引 `[]`、调用 `()`（含类型实参与尾部 lambda），从左向右接成链。
2. 前缀 `await`、`spawn`，从右向左。
3. 后缀传播 `?`。传播直接作用于前面的值；对传播后或等待后的值再接成员、索引或调用，写作 `(value?).member`、`(await task()).member`。
4. 前缀 `!`、`-`，从右向左。
5. `*`、`/`、`%`。
6. `+`、`-`。
7. `..` 与 `..<`，构造 `Range<Int>`；不连写。
8. `<`、`<=`、`>`、`>=`、`==`、`!=`、`is T`、`as? T`、`as dyn Trait`；本层不连写，区间判断写 `low < value && value < high`。
9. `&&`，再是 `||`（两级，各自左结合）。
10. `??`，右结合。
11. `=` 与复合赋值（如 `+=`）；不连写赋值，结果为 `Unit`。

`?.`、`??` 是完整记号，类型位置的 `T?` 是可空类型，都不是对表达式重复使用传播符。

`-1.toInt8Checked()` 先检查 `1.toInt8Checked()`，而一元负号不接受它的可空结果。负数本身作为接收者时写 `(-1).toInt8Checked()`。当负号直接作用于字面量时按带符号的值检查范围，因此 `-9223372036854775808` 和在 `Int8` 位置写 `-128` 都合法。

对 `record: Record?`，假设 `id: Int`、`name: String?`、`details: Option<Option<String>>`。`record?.id` 是 `Int?`，`record?.name` 是 `String?` 而不是额外套一层；若成员本来就是 Option，安全访问直接保留那个值。因此 `record?.details` 仍是 `Option<Option<String>>`：接收者为空产生 `None`；成员值为 `Some(None)` 则结果仍是 `Some(None)`。`??` 只拆掉左值的最外层：`Some(None) ?? Some("default")` 在明确的嵌套类型下得到 `None`，不会执行默认表达式。接收者不是 Option 时使用 `?.`，或对非 Option 使用 `??`，都是类型错误。

接收者在实参前求值，普通二元运算先左后右，赋值先确定并只求一次写入位置再求右值。`&&`、`||`、`??` 短路；`?.` 的接收者为空时也不求被跳过调用的实参。这些短路操作、基础赋值 `=`、成员访问 `.` 和传播 `?` 不可重载；其他运算符的协议和 `Eq` 派生见后续类型规范。

## 5. 控制流与清理

- 值位置的 `if` 必须有 `else`；各正常完成的分支须能得到目标类型。语句位置允许 `if id == null { return Response.status(400) }`。条件要求 `Bool`，不按数字或对象的“真假”转换。普通循环结果为 `Unit`；`while`、`for item in items` 支持不带值的 `break` / `continue`，只针对当前函数内最近的循环。
- `for item in items` 只求值一次输入，每轮建立新的只读绑定。对 `List<T>` 记录开始时的长度；随后追加的元素不延长遍历，修改尚未遍历的下标会在读取时可见。
- `start..<end` 和 `start..end` 是不可变的 `Range<Int>` 值，可以保存、传参、返回及参与编译期求值；两个端点从左到右各求值一次。区间公开只读的 `start`、`end` 和 `inclusive`，按这三个值比较、哈希。`for i in range` 按步长 `1` 迭代，闭区间包含终点；起点大于终点时不迭代，闭区间到达 `Int` 最大值后直接结束。
- `for character in text` 遍历 `String` 的 Unicode 标量（`Char`）；`for byte in bytes` 遍历 `Bytes` 的 `UInt8`。其他类型通过可见的同步方法 `iterator(): I` 和 `I.next(): Option<T>` 参与遍历，两个方法除接收者外不接收参数，也不声明类型形参。进入循环时调用一次 `iterator()`，每轮调用一次 `next()`；只有最外层的 `None` 才结束，`Some(None)` 仍是一个元素。协议按静态类型检查；有状态的游标可以用 Class 保存可变位置。`...` 的展开仍只接受 `List<T>`。
- 标准库的 [`HashMap` 和 `HashSet`](../std/collections/README.md#字典与集合) 通过这个协议遍历进入循环时的浅快照，分别产出键值项和集合元素，不承诺顺序。
- `match` 必须穷尽。被匹配值只求一次，按源码顺序选第一个模式和守卫都通过的分支。内置和类型和[封闭枚举](language-core.md#封闭枚举与匹配)可嵌套匹配，也可以匹配 `Bool`、整数、`Char` 和 `String` 字面量；整数范围参与穷尽性分析。未转义的顶层常量名按常量值匹配，反引号强制绑定。Struct/Class 使用具名字段模式；列表支持 `[]`、精确长度模式和末尾 `..rest`。`_` 忽略该位置，普通标识符创建只读绑定。用 `=>` 隔开模式和值，分支之间用逗号，允许末尾逗号。
- 模式支持 `|` 表示 OR，例如 `Text(value) | Failure(value)`；可嵌套在载荷和 Struct 字段内。每个备选分支必须绑定完全相同名称且类型一致的只读值。整数模式支持整数字面量端点的 `lower..upper`（含上界）和 `lower..<upper`（不含上界）；倒序或空区间不匹配任何值。浮点范围不属于模式，需用守卫表达。
- `match value { Point(x: 0, y: distance) => distance, _ => 0 }` 按字段名解构 Struct，不调用构造器；可以省去不关心的字段，显式解构的字段遵守访问权限。`Some(x) if x > 0 => x` 只在模式命中后求值 `Bool` 守卫，守卫为假就尝试下一分支；绑定仅在同一分支的守卫和结果中可见。有守卫的分支不计入穷尽检查。
- **已定：** 后缀 `?` 由操作数静态类型区分 `Err` 与 `None`，要求最近的函数或 lambda 返回兼容类型；对 `async fn` 检查其声明的完成值。不会把 `None` 自动变成 `Err` 或 HTTP 状态。[同步 `defer`](errors-and-resources.md) 在离开登记的块时倒序执行一次，包括 `return`、`?`、异常与协作式取消；`defer` 自身不可 `await`。

## 评审边界

[调用与解析正反例](0002-syntax-cases.md)列出首版验收用例。循环标签暂缓。HTTP 的 path/query/body 通过 [`std.http`](../std/http/README.md) 的 `Request` API 读取；可选 `#post` 宏生成同一 Router API 使用的 handler 适配器，不占用新的函数调用规则。
