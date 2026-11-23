# `std.json` 与 `#derive(Json)`

在有 `lyra.json5` 的包中，给结构体显式加上声明宏，即可生成当前模块私有的 `encode类型名`、`decode类型名` 两个入口。要跨包提供稳定 API，由库作者写普通 `pub fn` 包装。只写 `struct` 不会生成编解码器。

```lyra
#derive(Json)
struct User(pub val id: Int, pub val name: String)

fn main(): Unit {
    val text = encodeUser(User(7, "Ada"))
    match decodeUser(text) {
        Ok(user) => print(user.name),
        Err(reason) => print(reason),
    }
}
```

- 支持 `Int`（有符号 64 位）、`String`、`Bool`、同模块有编解码实现的嵌套结构体，以及这些类型的可空字段。数据中的多字节字符、引号和 NUL 都保留。解析拒绝重复键、无效 JSON、整数越界；派生器拒绝未知键，并用 `$.user.name` 这样的路径报告字段错误。
- `T?` 字段为 `None` 时省略键；缺键或 `null` 均解为 `None`。`Option<T?>` 区分缺键 `None`、显式 `null` 即 `Some(None)`、非空值 `Some(Some(value))`。更深的可空层没有不丢信息的 JSON 表示，要求手写映射。
- 当前派生范围为可通过公开字段构造的 `struct`。私有必填字段、`class` 身份图、浮点及自定义字段默认值没有自动映射。编码和解码都是普通 Lyra 函数，不需要运行时反射。
- 非固定 schema 可 `import std.json.parseJson`：它返回 `Result<JsonValue,String>`。用 `isObject()`、`isArray()`、`size()`、`key(index)`、`field(name)`、`item(index)` 检查结构，用 `readInt(path)`、`readString(path)`、`readBool(path)` 安全地读取标量。`missing()` 和 `isNull()` 可分别检查缺键和显式空值。

[标准派生器](../meta/macros/derive.ly)本身用 Lyra 编写，根据实际声明生成代码，与手写函数一起做名称解析和静态检查。底层采用构建机 `libjansson-dev` 中的静态 Jansson；部署 CLI 不需要安装 Jansson、Lyra 或 LLVM。
