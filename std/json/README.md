# `std.json` 与 `#derive(Json)`

在有 `lyra.json5` 的包中，给结构体写 `#derive(Json)`，即可在当前模块使用 `encode类型名`、`decode类型名` 和 `jsonCodec类型名()`。三者均为模块私有。

```lyra
#derive(Json)
struct User(pub val id: Int, [jsonName("display-name")] pub val name: String)

fn main(): Unit {
    val text = encodeUser(User(7, "Ada"))
    match decodeUser(text) {
        Ok(user) => print(user.name),
        Err(reason) => print(reason),
    }
}
```

这里编码得到 `{"id":7,"display-name":"Ada"}`。解码只认 `display-name`，Lyra 代码仍访问 `user.name`。两个字段最终映射到同一个键（无论是否都写了标记）会在 `#derive(Json)` 处报错，不等收到数据才发现。

- 支持下表的数值类型、`String`、`Char`、`Bool`、同模块有编解码实现的嵌套结构体、带显式编解码器的跨包字段、`List<T>` 及可空字段。字符串值中的多字节字符、引号和 NUL 都保留。派生的键名非空且不含 NUL，可含空格、点号、引号、换行或 `$`，会按 JSON 字符串规则编码。
- `Char` 在 JSON 中是恰含一个 Unicode 标量的字符串：`"🙂"`、`"\ud83d\ude42"`、`"\u0000"` 都能解码；`""`、`"ab"`、数字 `65` 和由 `e` 加组合重音构成的 `"e\u0301"` 会报字段路径错误。解码不做 Unicode 归一化。
- `decodeUser(text)` 默认拒绝未知键。只有接口明确允许对方添加字段时才调用 `decodeUserIgnoringUnknownFields(text)`：它在整个派生对象树忽略额外键，包括列表里的嵌套对象；重新编码只写已声明字段。已知字段的类型、必填要求不变，重复键（即使藏在忽略的字段内）、无效 JSON 和解析器范围外的整数仍报错。
- 错误路径使用输入中的键名：普通标识符用点号，其余键用方括号包住 JSON 字符串，如 `$.users[2]["display-name"]: expected String`。`$["user.name"]` 指一个带点的键，而 `$.user.name` 指两层对象。
- 对每个 `struct`，解码器先读取公开字段，再按声明顺序求缺键字段的构造默认值。默认表达式可引用前面已确定的字段，各执行一次。显式 `null` 只由字段类型解释，例如必填 `Int` 收到 `null` 会报字段路径错误。
- 若声明 `pub val name: String? = "guest"`，`{}` 解为 `"guest"`，`{"name":null}` 解为 `None`。编码这个 `None` 会写 `null`。单层可空字段没有默认值，或默认值直接写 `None` / `null` 时，编码 `None` 会省略键。其他默认表达式一律按可能产生非空值处理，编码不执行该表达式。
- `Option<T?>` 在没有默认值或默认值直接写 `None` / `null` 时区分三种状态：缺键得到 `None`，显式 `null` 得到 `Some(None)`，非空值得到 `Some(Some(value))`。这种字段若使用其他默认表达式，或字段有第三层可空类型，须手写映射。
- 私有字段必须有显式默认值。解码时由默认值初始化，编码时省略，类型可以没有 JSON 编解码实现。输入中的私有字段名按未知键处理。自动派生面向 `struct` 和 `enum`，`class` 身份图需手写映射。
- 非固定 schema 可 `import std.json.parseJson`：它返回 `Result<JsonValue,String>`。用 `isObject()`、`isArray()`、`size()`、`key(index)`、`field(name)`、`item(index)` 检查结构，用 `readString(path)`、`readChar(path)`、`readBool(path)` 或 `read` 加数值类型名（如 `readUInt16(path)`、`readFloat32(path)`）读取标量。`missing()` 和 `isNull()` 可分别检查缺键和显式空值。
- 手写 `read类型名Json(value: JsonValue, path: String)` 时，可把 `value.field(name)`、`value.item(index)` 直接交给其他解码器，它们继承当前视图的策略。若手动遍历未知键，按 `value.rejectsUnknownFields` 决定是否报错。`value.ignoringUnknownFields()` 与原视图共用解析结果，不修改原视图。

### 跨包复用编解码器

库可以从公开函数返回 `JsonCodec<T>`。例如，`models` 包对外提供 `User` 的 JSON 格式，库作者选择公开名称 `userWire`。

```lyra
import std.json.JsonCodec

#derive(Json)
pub struct User(pub val name: String)

pub fn userWire(): JsonCodec<User> { jsonCodecUser() }
```

使用方按普通模块规则导入类型和工厂。`[jsonCodec("userFormat")]` 将工厂绑定到字段里每一个 `Author` 值。`List`、可空状态和字段默认值仍由外层结构体处理。

```lyra
import models.User as Author
import models.userWire as userFormat

#derive(Json)
struct Article([jsonCodec("userFormat")] pub val authors: List<Author?>)
```

`JsonCodec<T>.encode(value)` 返回 JSON 文本。`decode(text)` 从 `$` 开始严格解码，`decodeIgnoringUnknownFields(text)` 允许未知键。`read(node, path)` 可直接解码已有 `JsonValue`，继承它的未知键策略，路径从传入的 `path` 继续。因此 `Article` 中第二位作者的字段错误会带上 `$.authors[1]`。这三个读取入口都返回 `Result<T,String>`。

手写格式可用 `std.json.jsonCodec<T>(encode, read)` 创建同一类型的编解码器。`encode` 的签名为 `(T) -> String`，`read` 的签名为 `(JsonValue, String) -> Result<T,String>`。字段标记指定的工厂须按零参数调用并返回该字段最内层元素的 `JsonCodec<T>`，编译时检查类型。

### 组合编解码器

直接组合现有 codec，不额外触发派生。例如 `jsonCodecList(userWire())` 处理 `List<User>`，`jsonCodecStringMap(userWire())` 处理 `HashMap<String,User>`。内置的 `jsonCodecString()`、`jsonCodecInt()` 等可充当叶子 codec。`jsonCodecResult(jsonCodecInt(), jsonCodecString())` 使用带 `tag` 和 `value` 的对象区分 `Ok` 与 `Err`。

显式 `jsonCodecOption(element)` 使用 `{"tag":"None"}` 或 `{"tag":"Some","value":…}`。这样即使 `element` 自己可空，也能无损区分 `None` 和 `Some(None)`；它与 Struct 派生中省略字段／使用 `null` 的约定不同。列表路径使用 `[index]`，Map 路径使用键，标签载荷路径使用 `.value`。Map 不限制输入键集合，但会把严格/宽松策略传给每个值；编码含 NUL 的键会抛异常。

派生泛型类型时，按声明顺序传入类型实参的 codec。比如 `struct Page<T>(pub val items: List<T>)` 派生后，可用 `jsonCodecPage(jsonCodecString())`，或通过公开函数返回 `JsonCodec<Page<T>>`。派生枚举 `enum Status { Active, Banned(String), Retry(Int, String) }` 分别编码为 `{"tag":"Active"}`、`{"tag":"Banned","value":"…"}` 或 `{"tag":"Retry","value":[3,"…"]}`。枚举载荷、`Result` 和字符串键 `HashMap` 可组合带标签的 Option codec。

### 数值怎样映射

| Lyra 字段类型 | 接受的 JSON 输入与失败行为 |
|---|---|
| `Int8`、`Int16`、`Int32`、`Int`/`Int64`、`UInt8`、`UInt16`、`UInt32` | 只接受不带小数点或指数的 JSON 整数，不把 `1.0`、`1e0` 或字符串 `"1"` 当成整数。落在目标类型范围外返回带路径的 `Err`，如 `$.rows[0]: out of range for UInt8`。 |
| `Float`/`Float64`、`Float32` | 接受 JSON 整数或小数。整数直接按目标精度舍入；小数先读为双精度，再按字段类型舍入。过大而舍入成无穷的 `Float32` 返回带路径的 `Err`；很小的数可舍入成次正规数或带符号的零。 |
| `UInt`/`UInt64` | 不自动派生。Jansson 的整数字面量范围是 `-9223372036854775808` 到 `9223372036854775807`；超出时在解析阶段报行列错误，不会退到浮点。需交换无符号 64 位全范围的协议应约定十进制字符串等显式表示。 |

有限浮点按其自身精度写出可往返的十进制；整数值的浮点保留小数或指数语法，`-0.0` 的符号也保留。JSON 没有 NaN 或正负无穷。为了让现有 `encode类型名(value): String` 调用保持简单，传入这些值（含列表元素、可空字段的非空值）会抛可捕获的 `Exception`，不会输出 `null`。手写编码时用 `float32Text(value)`、`float64Text(value)` 遵守同一规则；解码失败仍返回 `Result.Err`。

[标准派生器](../meta/macros/derive.ly)本身用 Lyra 编写，根据实际声明生成代码，与手写函数一起做名称解析和静态检查。底层采用构建机 `libjansson-dev` 中的静态 Jansson；部署 CLI 不需要安装 Jansson、Lyra 或 LLVM。
