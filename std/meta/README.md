# 编译期 Lyra

Lyra 宏以 Lyra 编写，入口返回 `Syntax`；编译器解析并检查生成物，运行程序时不加载宏实现。包私有的声明宏、表达式宏和装饰器分别放在 `src/.macros/`、`src/.expressions/` 和 `src/.decorators/`。同名宏和装饰器由使用位置区分：声明前写 `#name` 或 `@name`，表达式位置写 `#name(...)`。

```lyra
fn banner(target: String, argument: String): Syntax {
    syntaxCode("fn bannerText(): String { " + macroQuote(macroInput("message")) + " }")
}
```

宏实现本身先通过普通 parser 和类型检查，再由受限编译期求值器执行；它不会链接进业务程序。表达式宏的 `target` 为空，`argument` 是完整实参文本；声明宏和装饰器的 `target` 是目标名称，`argument` 是标记参数。调用程序使用的外部文件必须在清单的 `macroInputs` 中按宏名称显式授权。展开只读取这些快照，不联网、不启动准备脚本，也不能自行读取其他文件、环境变量或子进程。

## 语法节点

宏可用 `syntaxTarget()` 读取目标声明、`syntaxBody()` 读取未做类型检查的函数体、`syntaxArgument(i)` 读取实参；字段与形参仍可通过 `fieldCount()`、`fieldName(i)`、`fieldType(i)` 和字段标记查询 API 检查。通过 `syntaxChildCount(node)`、`syntaxChild(node, i)`、`syntaxKind(node)`、`syntaxText(node)` 遍历节点；`syntaxStart/End/Line/Column` 保留其原始源码位置。

使用 `syntaxCode(text)` 创建可信的 Lyra 源码片段；使用 `syntaxName`、`syntaxString`、`syntaxInt`、`syntaxCall`、`syntaxMember`、`syntaxBlock`、`syntaxJoin`、`syntaxConcat` 和 `syntaxArgs` 从节点构造输出。`syntaxArgument` 代表一个安全的语法插槽：只有将它放进输出代码，该实参才会在对应位置求值；映射括号也保留实参的来源位置。`syntaxError(node, message)` 把自定义错误指向被选中的源节点。生成代码进入正常名称解析、可见性、类型和泛型检查，因此宏不能绕过语言规则。

声明宏可通过 `#derive(Json)` 生成私有辅助实现；若希望生成公开 API，必须在调用处写 `#name(export apiName)`，且生成内容必须恰好提供该公开名称。不能通过生成物发布未点名的 API、`extern`、新的宏或公开重导出。宏能查看目标函数体，但不接收全程序类型检查后的 AST。标准库 JSON 和 TOML 派生器分别见 [`std.json`](../json/README.md) 与 [`std.toml`](../toml/README.md)。

## 装饰器

函数装饰器可返回 `syntaxAround(before, after, failure)`：`before` 在调用时先运行，`after` 只在返回成功后执行并可读取 `result: Any`，`failure` 只在异常时执行并可读取 `exception: Exception`，随后原样重抛。多层装饰器从外向内进入，按内向外处理返回值和异常。对异步函数，包装保留冷 `Task`：构造任务时不运行钩子，首次等待时执行包装体。

类型装饰器附着于 `struct` 或 `class`，读取其字段信息并生成同模块辅助声明；它不能修改原类型。若生成公开 API，调用处必须逐名授权，例如 `@makeBuilder(export buildUser)`；生成代码仍经过普通名称解析和类型检查。类型装饰器返回辅助声明代码，不使用函数装饰器的 `syntaxAround`。

```lyra
fn trace(target: String, argument: String): Syntax {
    syntaxAround(
        syntaxCall(syntaxName("print"), syntaxString("trace: " + target)),
        syntaxCode(""),
        syntaxCode("")
    )
}
```

## 输入快照与预算

例如表达式宏 `#query("SELECT ?1", id)` 可以把 `"SELECT ?1"` 与清单中的 SQL 快照核对，再用 `syntaxArgument(1)`、`syntaxArgument(2)` 拼出参数化调用。缺失或过期快照定位到相应源码；实参类型则由普通类型检查定位。实际 SQL 准备与在线校验由开发者显式运行独立工具，普通 `check/build` 不联网。

`macroQuote` 将文本编码成 Lyra 字符串字面量，`macroContains` 做大小写敏感、按字节的子串匹配，`macroInput` 读取授权快照。宏展开受嵌套深度、语法节点、源码字节数及求值步数限制；耗尽预算会给出宏诊断。表达式宏必须生成一个表达式，最多嵌套 32 层。**不要把 `syntaxCode` 的文本与 `syntaxArgument` 的源码节点混为一谈**：前者是宏作者构造的代码，后者保留实参作用域与错误映射。

示例 HTTP 的 `#post` 读取 `[path]`、`[query("tag")]`、`[body]` 标记，生成同模块的适配器与显式路由注册函数；调用业务 handler 仍经过其装饰器。标记本身不引入运行时反射，手写 handler 也使用相同的 Router API。
