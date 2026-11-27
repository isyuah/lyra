# 生成 Lyra 参考数据

```text
lyra doc ./my-package --locked --offline -o build/package-reference.json
lyra doc --stdlib -o build/lyra-reference.json
```

默认将 UTF-8 JSON 写到 stdout，诊断写到 stderr。指定 `-o` 时，只有生成成功才替换文件。包路径要求 `lyra.json5`，workspace 根目录会生成所有成员的公开接口。依赖解析和锁的行为与 `lyra check` 相同。生成器会展开宏并检查类型，不生成可执行文件，也不执行包的准备脚本。

## 写注释

在声明（或第一个宏、装饰器、属性前缀）前写连续、独占一行的 `///`。中间的空白行或普通注释会断开绑定。一行空的 `///` 用于分段；去掉 `///` 和其后至多一个空格，保留其余的 Markdown 缩进和代码块。普通 `//`、`////`、`/* … */` 不作为 API 说明。

```lyra
/// 返回输入值；省略参数时返回 `42`。
///
/// ```lyra
/// answer()
/// ```
pub fn answer(value: Int = 42): Int { value }
```

标准库的公开声明、字段和枚举分支都需要有说明，优先写清返回值、失败条件或使用限制，不必重复签名。隐式构造器和重导出默认沿用原声明的说明。编译器内置类型和操作的说明维护在 `compiler/check.cpp`，宏内置 API 则使用 `compiler/macros.cppm` 中宏前导的 `///`。用 `lyra doc --stdlib` 查看 `missingDocumentation`，`doc` 验收测试也会检查这一列表为空。

## JSON v1

- `schemaVersion` 是数据格式版本，`compilerVersion` 是实际生成数据的 Lyra 版本。
- `modules` 列出模块、所属包和包版本；`entries` 是 API 的扁平列表。用 `module` 分组，用 `ownerId` 聚合类型成员，用 `id` 做链接。ID 区分重载，不依赖源文件行号或形参的名字。将 ID 当作不透明字符串使用。
- `entries` 包含 `name`、`qualifiedName`、`kind`、`visibility`、`signature`、结构化 `parameters` 和 Markdown `documentation`。`source` 为相对所属包根目录的正斜杠路径，行列从 1 开始；内置条目的 `source` 为 `null`。宏生成的声明标记为 `generated`，定位到宏调用处。
- 重导出使用公开别名，并通过 `originId` 指向原声明。原声明来自依赖包时，原 ID 可能属于另一份参考数据。宏的 `signature` 显示调用名称，`implementationSignature` 显示编译期入口的实际签名；不同宏的调用语法应在其说明中写清楚。
- `missingDocumentation` 列出说明为空的条目 ID；这些条目仍保留签名。`--stdlib` 还输出 `syntax.keywords`、`syntax.contextualKeywords` 和 `syntax.operators`，运算符的 `precedence` 越大绑定越紧。

文档站可以直接读取 JSON，按模块、类型、成员分组，渲染签名与 Markdown 正文。教程、示例文章和页面布局由站点维护，不需要跟着生成参考数据一起改动。
