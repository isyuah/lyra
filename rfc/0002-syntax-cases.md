# RFC-0002：调用与解析正反例

这些是 [RFC-0002](0002-syntax.md) 的测试输入设计，不是测试运行报告。每行是独立用例；`↵` 表示源码中的真实换行。所有用例适用于 Debug / Release。正例看类型、结果或可观察的副作用；反例检查错误原因及主源码位置，不锁死文案。

[语法命令行用例](../tests/syntax_cli.py)直接验证关键字与反引号、LF/CRLF、调用尖括号、列表展开和前后缀优先级。拒绝用例使用原始 UTF-8 字节范围定位 token。

## 一个回调，加多少个实参？

沿用正文的两个函数：`visit` 的末尾形参是 `values: Int...`；`visitList` 的 `values: List<Int>` 是固定形参。`printInt` 记录传入的整数，`ids: List<Int>` 含 `2, 5`。`makeIds()` 记录一次调用并返回 `ids`。

| ID | 输入 | 预期 |
|---|---|---|
| S2-001 | `visit(printInt)` | 通过；没有记录。回调是第一个固定实参，变长段为空。 |
| S2-002 | `visit(printInt, 2, 5)` | 通过；记录 `2, 5`。 |
| S2-003 | `visit(printInt, 1, ...makeIds(), 8)` | `makeIds()` 只求值一次；记录 `1, 2, 5, 8`。 |
| S2-004 | `visit(printInt, ids)` | 列表作为一个实参，在 `ids` 报期望 `Int`、实际为 `List<Int>`；若想逐项传入，提示写 `...ids`。 |
| S2-005 | `fn bad(values: Int..., onValue: (Int) -> Unit): Unit {}` | 在 `onValue` 报变长形参之后不能再声明形参，关联 `values`。 |
| S2-006 | `visit(printInt, 2) { printInt(it) }` | 在尾部 `{` 报无固定的末尾函数形参，不把块追加到变长段。 |
| S2-007 | `visitList(values: ids) { printInt(it) }` | 与 `visitList(ids, printInt)` 都记录 `2, 5`。 |
| S2-008 | `visitList(ids, printInt) { printInt(it) }` | 在尾部 `{` 报 `onValue` 已绑定，并指出先前的实参。 |
| S2-009 | `visit(printInt, values: ids)` | 在 `values:` 报变长形参不接受命名实参，不隐式展开。 |
| S2-026 | 用同一个 `countArgs(values: Any...)` 数变长段的实参：`countArgs(ids)`；`countArgs(...ids)` | `ids` 是 `[2, 5]` 时分别得到 `1` 和 `2`；换成空的 `List<Int>` 时分别得到 `1` 和 `0`。 |
| S2-030 | `fn takeMany(...values: Int): Unit {}` | 在 `...` 处提示声明应写 `values: Int...`；改正后 `takeMany()`、`takeMany(2, 5)` 和 `takeMany(...ids)` 均可通过。 |
| S2-031 | 对 `fn takeList(values: List<Int>): Unit {}` 分别调用 `takeList(ids)` 和 `takeList(...ids)` | 前者通过；后者在 `...` 处报没有可接收展开元素的变长形参。 |
| S2-035 | `cursor` 是逐项产生 `Int` 的自定义游标，不是列表。对 `countArgs(values: Any...)` 分别调用 `countArgs(cursor)`、`countArgs(...cursor)`。 | 前者把游标本身作为一个值，得到 `1`；后者在 `...cursor` 处编译失败，提示只能展开 `List<T>`，不尝试遍历游标。 |

## 变长参数：改动谁能看见？

```text
fn setFirst(items: List<Int>): Unit { items[0] = 9; }
fn changeFirst(values: Int...): Int { setFirst(values); values[0] }

class Contact { var name: String }
fn renameAndReplace(replacement: Contact, values: Contact...): Contact {
    values[0].name = "Ada"
    values[0] = replacement
    values[0]
}
```

| ID | 输入 | 预期 |
|---|---|---|
| S2-032 | `ids` 初值为 `[2, 5]`。先调用 `changeFirst(...ids)`，再调用 `setFirst(ids)`。 | 前者返回 `9`，此时 `ids` 仍为 `[2, 5]`；后者直接修改传入的列表，`ids` 变为 `[9, 5]`。 |
| S2-033 | `contacts` 的第一项是名为 `"Lin"` 的 `original`；`other` 是另一个对象。调用 `renameAndReplace(other, ...contacts)`。 | 返回 `other`，但 `contacts[0]` 仍是 `original`，其 `name` 已变为 `"Ada"`。 |
| S2-034 | `fn collect(values: Int...): List<Int> { values }`；保留两次 `collect()` 的结果，只向第一份追加 `7`。 | 第一份含 `7`，第二份仍为空。即使都没有实参，两个调用也不共享可变列表。 |

## 默认值与泛型

`mark(label, value)` 先记录 `label`，再返回 `value`。`identity`、`absent` 的定义见正文；`draw<T: Render>` 要求实例方法 `render(): String`，而夹具 `WrongBadge` 的 `render` 返回 `Int`。

```text
fn join(left: Int = mark("L", 1), right: Int = mark("R", left + 1)): Int {
    left * 10 + right
}

fn lookup<Key, Value>(key: Key): Value? { null }
```

| ID | 输入 | 预期 |
|---|---|---|
| S2-010 | `join(right: mark("A", 4), left: mark("B", 2))` | 记录 `A, B`，结果 `24`；两个默认值都不执行。 |
| S2-011 | `join(right: mark("X", 9))`；另测 `join()` | 前者记录 `X, L`，结果 `19`；后者记录 `L, R`，结果 `12`。 |
| S2-012 | `join(right: 4, 2)`；另测 `join(2, left: 4)` | 分别在 `2` 报命名实参后有位置实参、在 `left:` 报重复绑定。 |
| S2-013 | `identity(7)`；`identity<Int>(7)` | 前者从实参推得 `T = Int`，后者显式指定 `Int`；均得到 `Int` 值 `7`。`7 < identity(8)` 则是比较，得到 `true`。 |
| S2-014 | `val name: String? = absent()`；另测 `val value = absent()` | 前者推得 `T = String`，结果是 `String?` 的 `None`。后者在 `absent()` 处报无法推断 `T`，不取 `Any`；改写为 `absent<String>()` 或给 `value` 加 `: String?` 均可通过。 |
| S2-015 | `draw(WrongBadge())` | 在实参处指出要求 `render(): String`，实际返回 `Int`，关联约束声明。 |
| S2-027 | `left < right`（值分别为 `2`、`5`）；另测 `left<right>(7)`（`left` 与 `identity` 签名相同，`right` 只声明为值） | 前者比较得到 `true`；后者在类型实参 `right` 处报需要类型，不把调用重解析为比较。 |
| S2-029 | `val score: Int? = lookup("Ada")`；另测 `val score = lookup("Ada")` | 前者从实参得到 `Key = String`、从目标类型得到 `Value = Int`，结果为 `None`。后者仍能确定 `Key`，在调用处报无法推断 `Value`。 |

## 从源码能否唯一读出控制流？

设 `ready(): Bool` 返回 `true`，`start()` 记录一次调用；`items` 中至少一项的 `active` 为真，`any` 接收单参数谓词。`findUser` 与[小服务](../examples/user-service/src/main.ly)签名相同。涉及 `await` 的表达式放在声明返回 `Result<User?, DbError>` 的异步函数内。

| ID | 输入 | 预期 |
|---|---|---|
| S2-016 | `fn total(): Int { 1 +↵2↵}`；另测 `fn total(): Int { 1 + 2; }` | 前者返回 `3`；后者在块末的 `;` 提示结果已丢弃，得到 `Unit` 而非 `Int`。 |
| S2-028 | 分别调用 `fn discard(): Unit { mark("D", 3); }` 和 `fn keep(): Int { return mark("K", 3); }` | 前者记录一次 `D`，块值为 `Unit`；后者记录一次 `K`，显式返回 `3`。 |
| S2-017 | `val total = 1↵+ 2` | 在 `+` 报错；把 `+` 移到上一行末尾或把加法放进括号。 |
| S2-018 | `if ready() { start() }`；`if (items.any { it.active }) { start() }` | 两段各调用 `start()` 一次；前者没有给 `ready` 传回调，后者只有内层块是 lambda。 |
| S2-019 | `val count: Int = if ready() { 1 }` | 在 `if` 表达式报值位置缺 `else`。 |
| S2-020 | `fn status(id: Int?): Int { match id { Some(value) => 200, } }` | 在 `match id` 报漏掉 `None`。 |
| S2-021 | `val value = await findUser(7)?` | 与 `(await findUser(7))?` 同义，`value: User?`；`Ok(None)` 不提前返回，`Err` 才传播。 |
| S2-022 | `val value = await (findUser(7)?)` | 在 `?` 报实际为 `Task<Result<User?, DbError>>`，应先 `await`。 |
| S2-023 | `defer await findUser(7)` | 在 `await` 报同步清理不能挂起，即使外层函数是异步也一样。 |
| S2-024 | `fn scaled(): Int { val _slot2 = 1_000; _slot2 + 2 * 3 }`；另测 `val count = 2 /* note` | 前者返回 `1006`；后者在 `/*` 报块注释未闭合。 |
| S2-036 | `-1.toInt8Checked()`；另测 `(-1).toInt8Checked()` 和 `-9223372036854775808` | 首例的成员调用先结合，一元负号收到 `Int8?` 而出错；后两例分别得到 `Some(-1)` 和最小 `Int`。 |
| S2-037 | `identity<Int> (7)`；另测 `value? .size` | 首例的 `>` 未与 `(` 相邻，不能识别为泛型调用；后者提示用 `(value?).size` 明确接收者。 |
| S2-038 | `Text(value) | Failure(value)`，两分支的 `value` 类型相同；另测 `true | false` | 两个备选共用一个只读绑定；`true | false` 穷尽 `Bool`。OR 可嵌套在枚举载荷与 Struct 字段模式中。 |
| S2-039 | 对 `Int8` 匹配 `-128..-1` 和 `0..127`；另测 `-10..<0` 与 `0..2` | 首例穷尽整个 `Int8` 域；闭区间包含上界，`..<` 不包含上界。 |
| S2-040 | `Left(item) | Right(item)`，其中载荷分别为 `Int` 和 `String`；另测 `Left(item) | Right(other)` | 两例都在 OR 模式处拒绝：备选必须绑定相同名称且类型一致。 |
| S2-041 | `Int8` 只匹配 `-128..0` | 在 match 处报告缺失值 `1`；范围参与穷尽性分析，而不是退化成 `_`。 |
| S2-042 | `Gate(open: false)`、`Gate(open: true)` 和 `_` 匹配 `class Gate(val open: Bool)`；另测只匹配 `Gate(open: true)` | Class 使用具名字段解构且按字段判断分支；Class 模式不参与穷尽推断，因此缺少 `_` 时报告不穷尽。 |
| S2-043 | `if let Some(text) = value { text } else { "empty" }`，并分别传入 `Some("ready")` 与 `None` | 成功时绑定只在 then 分支可见；subject 仅求值一次，失败时进入 else。无 else 的 `if let` 在 `Unit` 语境下不执行失败分支。 |
| S2-044 | `val Point(x: x) = point`；`val Some(x) = nullable` | 前者声明可供后续语句使用的 `x`；后者在模式处报 `LYT0012`，因为不能覆盖 `None`。 |
| S2-045 | `val range: Range<Int> = 1 + 1..<2 * 3`；`items.slice(range)` | 构造从 `2` 到 `6` 的半开区间，支持直接传参和 `for`；切片复制下标 `2` 到 `5` 的元素。`0..1..2` 不可连写。 |
| S2-044 | `match values { [] => ..., [first] => ..., [first, ..rest] => ... }` | 空、精确长度和末尾 rest 均按列表长度匹配；rest 是独立浅拷贝。`[]` 与 `[first, ..rest]` 穷尽任意 `List<T>`；只写 `[first]` 不穷尽。 |
| S2-045 | `const Timeout: Int = 3`；分别匹配 `Timeout` 与 `` `Timeout` `` | 前者按常量值匹配；后者创建局部绑定，可在该分支中读取。 |

**S2-025 · 提前返回只退出当前 lambda。** `readInt()` 总返回 `Err`，`note` 记录字符串。下面的 `probe()` 须返回 `9`，并依次记录 `second, first, after`。把 `Ok(readInt()?)` 换成 `return Ok(3)` 后，同样记录三项并返回 `3`。外层声明返回 `Int`，并不是 `?` 或 `return Ok(3)` 要匹配的返回类型。

```text
fn runOnce(action: () -> Result<Int, DbError>): Result<Int, DbError> { action() }

fn probe(): Int {
    val result = runOnce {
        defer note("first")
        defer note("second")
        Ok(readInt()?)
    }
    note("after")
    match result { Ok(value) => value, Err(_) => 9, }
}
```

**可运行的循环与可空反例：** `1?.size`、`1 ?? 2` 在运算符处报需要 Option；`Some(3) ?? "wrong"` 在默认值处报预期 `Int`。若 `outer: Option<Option<Int>>` 为 `Some(None)`，`outer ?? Some(7)` 仍为空的 `Int?`，不是 `7`。对空接收者使用 `receiver?.method(next())` 时不会调用 `next()`。`for item in 7 {}` 在 `7` 处报需要 List；对应[运行测试](../tests/sugar_cli.py)也覆盖泛型列表、空列表、遍历时追加和修改元素，以及 `continue` 与 `break`。

HTTP 层另用[混合输入和四条服务路径](../examples/user-service/README.md)验收。把 `?tag=a&tag=b` 解成列表、把一个 JSON 对象解成一个 body 参数，是库的输入契约，不是 S2-002 的函数变长绑定。
