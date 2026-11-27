# 编译期 Lyra

Lyra 宏以 Lyra 编写，入口返回 `Syntax`；编译器解析并检查生成物，运行程序时不加载宏实现。普通声明宏、类型宏、表达式宏和装饰器分别放在源码模块的 `.macros/`、`.typedMacros/`、`.expressions/` 和 `.decorators/`。声明前写 `#name` 或 `@name`，表达式位置写 `#name(...)`；普通声明宏与类型宏在同一作用域不能同名。公开入口用 `pub fn`，下游包可以写 `import macro kit.name as nameHere` 或 `import decorator kit.trace`；加 `pub import` 可重导出。

```lyra
fn banner(target: String, argument: String): Syntax {
    syntaxCode("fn bannerText(): String { " + macroQuote(macroInput("message")) + " }")
}
```

宏实现本身先通过普通 parser 和类型检查，再由编译期求值器执行。可导入手写的纯同步函数、常量和类型；不能依赖生成声明或装饰后的函数体。表达式宏的 `target` 为空，`argument` 是完整实参文本；声明宏和装饰器的 `target` 是目标名称，`argument` 是标记参数。宏读取的外部文件必须由调用包在 `macroInputs` 中按使用处名称显式授权；改用别名时用别名配置。展开只读取这些快照，不联网、不启动准备脚本，也不读取其他宿主文件或环境变量。

## 语法节点

宏可用 `syntaxTarget()` 读取目标声明、`syntaxBody()` 读取未做类型检查的函数体、`syntaxArgument(i)` 读取实参；字段与形参仍可通过 `fieldCount()`、`fieldName(i)`、`fieldType(i)` 和字段标记查询 API 检查。通过 `syntaxChildCount(node)`、`syntaxChild(node, i)`、`syntaxKind(node)`、`syntaxText(node)` 遍历节点；`syntaxStart/End/Line/Column` 保留其原始源码位置。

`syntaxDoc(node: Syntax): String` 读取声明、字段、形参或枚举分支附带的 `///` Markdown，没写则返回空字符串。文档放在声明或它的第一行前缀之前，可以跨过整个多行装饰器或属性。

`syntaxCommentCount(node: Syntax): Int` 和 `syntaxComment(node: Syntax, index: Int): Syntax` 按源文件顺序查询节点范围内的注释（含 `///`）。注释不占原有子节点下标；取出的节点 `syntaxKind` 为 `comment`，用 `syntaxText` 读取原文，用位置查询定位到源码。纯生成节点没有源码注释，下标越界会报告宏诊断。

显式类型标注可传给 `syntaxResolvedType(annotation)`。返回的只读节点展开透明别名，跟随导入和重导出，保留类型形参。`syntaxKind` 区分 `builtin`、`standard`、`named`、`parameter`、`function`、`projection` 和 `dyn`，`syntaxText` 读节点名称，`syntaxTypeModule` 读所属模块，子节点表示类型实参。`dyn` 下的 `binding` 子节点名称是关联类型名，唯一子节点是绑定的类型。生成代码时使用原标注的 `syntaxText` 或语法拼接，不直接输出已解析的节点。`syntaxCodeAt(node, text)` 让生成片段的诊断定位到传入节点；名称节点可用 `syntaxIdentifier(node)` 读取去掉反引号的标识符。泛型声明的 `typeParameterDeclarations()` 保留约束，`typeParameterArguments()` 只列形参名。

使用 `syntaxCode(text)` 创建可信的 Lyra 源码片段；使用 `syntaxName`、`syntaxString`、`syntaxInt`、`syntaxCall`、`syntaxMember`、`syntaxBlock`、`syntaxJoin`、`syntaxConcat` 和 `syntaxArgs` 从节点构造输出。`syntaxArgument` 代表一个安全的语法插槽：只有将它放进输出代码，该实参才会在对应位置求值；映射括号也保留实参的来源位置。`syntaxError(node, message)` 把自定义错误指向被选中的源节点。生成代码进入正常名称解析、可见性、类型和泛型检查，因此宏不能绕过语言规则。

也可用 `quote` 写模板，用 `splice` 插入节点。模板自己引入的局部名称与调用处隔离，插入的调用实参保留原绑定和位置。

```lyra
pub fn increment(target: String, argument: String): Syntax {
    val input = syntaxArgument(0)
    quote { splice(input) + 1 }
}
```

声明宏可通过 `#derive(Json)` 生成私有辅助实现；若希望生成公开 API，必须在调用处写 `#name(export apiName)`，且生成内容必须恰好提供该公开名称。不能通过生成物发布未点名的 API、`extern`、新的宏或公开重导出。宏能查看目标函数体，但不接收全程序类型检查后的 AST。标准库 JSON 和 TOML 派生器分别见 [`std.json`](../json/README.md) 与 [`std.toml`](../toml/README.md)。

## 类型宏

将下面的实现保存为 `src/.typedMacros/requireString.ly`，即可用 `#requireString` 标记普通函数或方法。`syntaxResultType()` 读取实际结果，包括省略注解时的推断结果；`syntaxTypeOf(node): Syntax?` 可读取目标表达式、局部绑定或签名节点的类型。它们返回与 `syntaxResolvedType` 同样的只读节点。

```lyra
fn requireString(target: String, argument: String): Syntax {
    val result = syntaxResultType()
    if syntaxKind(result) != "builtin" || syntaxText(result) != "String" {
        return syntaxError(syntaxBody(), "expected a String result")
    }
    syntaxCode("")
}
```

普通宏和装饰器先展开，再检查类型宏的目标；目标不能依赖类型宏尚未生成的帮助函数。类型宏可以产生私有辅助声明或显式授权的公开声明，也可以只检查并返回空输出。泛型保留符号类型，例如 `T`，不因调用者传入了 `Int` 而变成 `Int`。

## 装饰器

函数和实例方法装饰器可返回 `syntaxAround(before, after, failure)`：`before` 先运行，`after` 只在成功后执行并可读取 `result: Any`，`failure` 可读取 `exception: Exception`。包装器重抛原异常，异常钩子自身失败时附加到 `cleanupFailures`。方法钩子可通过 `this` 读取接收者。多层装饰器从外向内进入，按内向外处理返回值和异常。对异步函数，包装保留冷 `Task`：构造任务时不运行钩子，首次激活时执行包装体。

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

示例 HTTP 的 `#get` / `#post` 读取 `[path]`、`[query("tag")]`、`[body]` 和 `[inject]` 标记，生成私有适配器。`#mountRoutes` 由编译器按包汇总，并把适配器一次交给 Router。调用业务 handler 仍经过装饰器；运行时不扫描声明。
