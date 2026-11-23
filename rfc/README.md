# Lyra RFC Modules

本目录收录 RFC-0001 经过讨论后确认的模块边界，以及每个模块仍需补充的规范细节。专题文档对各自列出的已接受行为作准；[总览草稿](../RFC_draft.md)保留原有讨论、候选语法、跨模块问题和后续 RFC 规划。

“已接受”表示设计方向已由作者确认，可以据此设计一致性测试；**不表示编译器已实现，也不表示此模块不再需要更细的语法或 ABI 规范**。遇到未定事项应保留为 Open，不能根据示例自行扩展承诺。

| 模块 | 文件 | 范围与状态 |
|---|---|---|
| RFC-0002：词法、表达式、函数与控制流 | [正文](0002-syntax.md) · [调用与解析正反例](0002-syntax-cases.md) | 变长列表、泛型调用、lambda、换行、运算符优先级和求值顺序已定；`await` 先于 `?` |
| RFC-0003：类型系统、泛型与 Trait | [重载与隐式转换](0003-types.md) | 无名转换按类型对查找；多实参推断、数值及可空提升、默认与变长形参的重载平局规则已定 |
| 语言核心与类型 | [language-core.md](language-core.md) | 返回标注 `: T`、块末表达式、参数绑定、引用与闭包、`Any`、空值、扩展方法与可见性 |
| 泛型与接口 | [generics-and-interfaces.md](generics-and-interfaces.md) | 首版泛型函数、编译期结构约束、静态成员解析、泛型语义与代码生成分离 |
| 错误与资源 | [errors-and-resources.md](errors-and-resources.md) | `Result`、`?` 复用普通 `implicit`、异常与取消、资源别名、关闭后的错误和同步 `defer` |
| 异步执行 | [async-execution.md](async-execution.md) | 冷 Task 重复等待、任务组收尾、同域串行、显式跨域共享、竞争与内存安全 |
| 模块、包与构建 | [modules-packages.md](modules-packages.md) | 文件与模块映射、可见性、re-export、包身份、锁文件、源码发布与显式脚本授权 |
| 原生工具链与 FFI | [native-toolchain.md](native-toolchain.md) | Windows 与 Linux 原生构建和 AOT 交付、GC 演进路线、SQLite 句柄与取消验收 |
| 编译器与运行时验收 | [compiler-acceptance.md](compiler-acceptance.md) | H 委托设计：先交付源码诊断与最小 AOT 正反例，再验证 GC、异步、FFI 与服务 |
| 元编程与数据交换 | [metaprogramming-data.md](metaprogramming-data.md) | `[标记]`、`@装饰器`、`#宏` 分写；声明宏与表达式宏、只读输入、独立授权的外部工具和 JSON/TOML 显式派生 |

## 第二轮取舍：GC 语言的日常写法

设计优先级是业务代码好写、错误好定位，再优化资源占用和执行速度。语言 RFC 定义程序员能观察到的行为；不要因为 FFI 要管理一个指针，就要求所有调用者学习所有权，也不要因为后端要正确追踪引用，就限制业务对象跨域。以下说明如何解读[原始答卷](../design/05-rfc-completion.md)：

| 答卷或复核 | 采用的规则 |
|---|---|
| `TYPE-07=B` 加自定义转换；返回值与普通调用复核 | 支持无损数值提升和无名的用户隐式转换。返回分支见[语言核心](language-core.md)的 `LANG-007`；重载、跨模块查找及泛型调用的转换位置见 [RFC-0003](0003-types.md) 的 `T3-001` 至 `T3-019`。 |
| `TYPE-06` 有保留；本轮撤回原题 A 的推荐 | 文件和连接是普通引用，别名共同观察关闭。用 `defer`/作用域 API 释放外部资源；可选漏关提示不改变类型。不引入通用 `Owned<T>`、移动/借用检查或生命周期参数。 |
| `ASYNC-01=C`、`ASYNC-02=A` | 显式并行可共享普通可变对象；要保证并发更新的结果就同步，不要求 `Send`/`Sync` 或等价资格。普通 `spawn` 仍同域串行，任务组仍要等子任务清理结束。 |
| `ASYNC-03` 原题未答；按后续复核与 D1 处理 | `Task<T>` 重复等待按普通值/引用语义取结果，不为资源引入 `Copy` 或单消费者 Task。可直接跨域 `await`，在等待者域恢复；等待者取消不顺带取消其他组的任务。 |
| `SYN-07` 的追问 | 扩展按模块可见性访问私有成员。 |
| `META-04=B 或 C`，以本轮 JSON 取舍为准 | 普通记录用 `#derive(Json)` 显式请求生成编解码代码；已有实现可直接使用。运行时反射可另设入口，不替静态调用兜底。 |
| `NAT-02=A → B`、`RFC-02=A` | 先评估成熟保守 GC，长期走精确根追踪。LLVM 版本、收集器、默认线程/阻塞池容量等交给工具链和原型，不阻塞语言 RFC。 |

设计验收用一个按 ID 查 SQLite 并返回 JSON 的小 HTTP 服务：[Lyra 源码](../examples/user-service/src/main.ly)和[四条请求路径](../examples/user-service/README.md)展示命中、缺失、失败与取消。样例中的库 API 是候选，语言保证仍以各专题及[总览 §9](../RFC_draft.md)为准。

## 集中评审结论

[集中确认单](../design/06-v1-batch-review.md)的 16 项已处理：13 项按原推荐接受，H 委托设计，最后两项按复核后的推荐定案：

- [C1 · `?` 复用普通 `implicit`](errors-and-resources.md#c1-conversions)：不同具体错误类型之间只查一条直接转换；缺失或歧义在 `?` 处报告。
- [F1 · 声明宏、表达式宏与外部输入](metaprogramming-data.md#f1-macro-inputs)：宏按声明的只读输入离线展开；连库和外部生成器由显式授权的独立工具处理。

具体收集器、GC 根追踪、接口装箱、沙箱实现等交给原型验证。后续版本的完整反射、关联类型、静态空镜像和私有包中心不阻塞[首个编译器切片](compiler-acceptance.md)。
