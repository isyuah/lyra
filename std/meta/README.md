# 编译期 Lyra

将本包私有的声明宏放在 `src/.macros/<名称>.ly`，函数入口是 `fn 名称(target: String, argument: String): String`。`#名称` 或 `#名称(标识符)` 标在声明前，返回同模块的 Lyra 声明源码。宏文件自己也先经过常规 parser 和类型检查，但只在受限的编译期求值器中执行，不链接进应用。标准库 [`#derive(Json)`](../json/README.md) 是完整实例。

```lyra
// src/.macros/banner.ly
fn banner(target: String, argument: String): String {
    "fn bannerText(): String { " + macroQuote(macroInput("message")) + " }"
}
```

上例在清单里授予该宏唯一的输入：`macroInputs: { banner: { message: 'inputs/banner.txt' } }`。在 `fn main()` 前写 `#banner` 后，同模块即可调用 `bannerText()`。输入变更后重新 `check` 或 `build` 会重新展开；缺文件或权限时不会执行准备脚本。发布时仅额外带上 `inputs/` 下显式声明的文件。

- `targetKind()` 区分 `fn`、`struct` 和 `class`；`targetBody()` 读取被修饰函数自身尚未类型检查的块源码。`fieldCount()`、`fieldName(i)`、`fieldType(i)`、`fieldPublic(i)` 读取字段或函数形参；`optionInner(typeName)` 和 `listElement(typeName)` 分别拆一层可空和列表类型。`macroQuote(text)` 生成 Lyra 字符串字面量，`macroFail(message)` 报告调用处错误。
- `#post("/teams/{id}")` 可以读到路由字面量，以及形参上的 `[path]`、`[query("tag")]`。`fieldHasAttribute(i, "query")` 检查标记是否存在，`fieldAttributeValue(i, "query")` 得到 `tag`；无参数标记返回空字符串，读取不存在的标记报错。字段也可在 `pub val` 前写标记。标记自身不进行取参；宏生成的适配器和手写调用一样走原函数和装饰器，无需运行时反射。
- 求值器支持普通同步函数、标量、局部绑定、分支、循环和字符串拼接。每次展开限 50,000 步、64 层函数调用、1 MiB 单次文本和 32 MiB 累计文本预算。不能用 `extern`、系统 I/O、环境变量、网络或子进程。输出重新解析和检查；宏不能自行发布 API、声明 `extern` 或递归生成新的声明宏。生成代码的私有导入不会占用手写代码的名称。
- 本包入口装饰器放在 `src/.decorators`；返回的语句在调用时进入原函数体。多个装饰器按源码从上到下进入。标准 `@trace` 装饰冷 Task 时，构造 Task 不会打印；首次真正运行时打印，重复等待不重复打印。
- 目前接口以限定范围的源码文本和声明查询为主。完整 AST 构造、任意签名不变的前后包装和表达式宏的最终契约见[宏 RFC](../../rfc/metaprogramming-data.md)。
