# Lyra RFC：语言核心与类型

**状态：已接受核心设计方向；具体语法和部分类型规则仍开放**  
**范围：** 表达式与函数、绑定可变性、对象模型、类型契约、空值和基本算术。调用、词法与控制流的详细草案见 [RFC-0002](0002-syntax.md)；函数装饰器及数据序列化见[元编程与数据 RFC](metaprogramming-data.md)。

## 表达式与函数

- 分号可省。块正常结束时，末尾表达式的结果就是块值；加 `;` 则丢弃结果，块值为 `Unit`。`return expr;` 仍显式返回 `expr` 的值。`if` 在值位置必须有 `else`；`match` 对封闭枚举和 `Option` 必须穷尽，普通循环返回 `Unit`。
- 具名函数的参数类型显式声明。局部变量、lambda 和私有非递归函数的返回类型可按上下文推导；公开函数和递归函数必须声明返回类型。
- 私有函数和方法可写 `fn count(items: List<Int>) { items.size }`。推断只使用函数体：显式 `return` 和块尾的正常路径检查同一结果类型，空块或丢弃尾值的块产生 `Unit`。调用者的目标类型不改变推断结果；`None`、单独的 `Ok` / `Err` 等缺少必要类型信息时，需要显式提供类型。入口、`extern` 和 `implicit` 也要求标注；通过静态调用形成递归环的函数同样需要标注。
- 支持命名参数、默认参数和变长参数；命名参数可以按名称调整顺序。
- 调用实参按照源码中从左到右的顺序，各求值一次，再绑定到对应形参。
- 写出具名函数返回类型时，参数列表之后使用 `: T`。
- Lyra 自有名字采用驼峰：函数、方法、变量、形参和字段用 `lowerCamelCase`，类型、trait 和枚举分支用 `UpperCamelCase`。这是风格约定，不改变标识符文法、外部数据键名或 C ABI 符号。

```text
fn title(score: Int): String {
    if score >= 60 { "通过" } else { "未通过" }
}

val result = join(right: mark("R"), left: mark("L"))
```

`mark("R")` 必须先于 `mark("L")` 求值，前者的结果绑定到 `right`。位置实参在命名实参之前，每个固定形参最多绑定一次。缺省的默认表达式按形参声明顺序求值；引用形参时只能引用更早的形参，实例方法还可访问接收者。

变长形参写 `name: T...`，必须在最后，接收零个或多个位置实参，逐个按元素类型 `T` 检查。函数体中 `name` 是本次调用独立的可变 `List<T>`。调用处的 `...` 首版只展开列表：`...ids` 不共用原列表，但引用元素仍指向同一对象。固定形参 `name: List<T>` 则直接接收调用者传入的 `ids`，两边访问同一个列表。见 [RFC-0002 正反例](0002-syntax-cases.md)。

## 绑定与对象

- `Struct` 是值类型。复制时，值字段独立复制、引用字段复制引用；不递归克隆整个对象图。
- `Class` 是引用类型。赋值复制引用并保留对象身份，普通实例由 GC 管理内存。
- 使用 `val` 表示不可重新赋值的绑定，使用 `var` 表示可重新赋值的绑定；不采用含义相同或相反的 `let` 别名。
- 通过 `val` 不能重赋值绑定或修改值类型字段；若 `val` 持有 Class 引用，仍能经该引用修改对象声明为可变的字段。
- 不支持带状态的 class 单继承；优先通过组合复用实现。

```text
struct Profile { var name: String; var owner: User }
class User { var name: String }

val first = Profile(name: "原稿", owner: user)
var copy = first
copy.name = "修订稿"
first.owner.name = "Ada"   // 通过 Class 引用修改对象
// first.name = "定稿"      // 错误：val 的值字段不可修改
```

`File`、`Connection` 等外部资源是普通引用对象；即使放进 Struct，复制的也只是引用。安全代码不需要用所有权类型、生命周期参数或借用检查来表达关闭责任；[资源清理](errors-and-resources.md)定义别名与关闭的行为。普通对象的跨域传递遵循[异步执行 RFC](async-execution.md)，不以类型标记为条件。

可变 `List` 赋值共享同一个集合；经一个别名修改元素，其他别名能看到。闭包捕获绑定本身，不暗中复制当前值；首版 `return` 不从调用该闭包的外层函数返回。

`==` 和 `!=` 对 `Int`、`UInt`、`Float32/64`、`Bool`、`Char`、`String`、`Bytes` 与 `Unit` 提供内置 Eq；浮点采用 IEEE 值相等（`NaN != NaN`、`+0 == -0`）。Class 固定按对象身份相等，身份不是可观察的固定地址；List、Task、TaskLocal、CPtr 和 Exception 也按其运行时身份相等。`Any`、`dyn` 和闭包没有默认 Eq。

Struct 默认按全部字段递归比较；Enum 按活动分支和载荷比较；`Option` 按空/非空标签及值比较，`Result` 按 `Ok` / `Err` 及对应载荷比较。比较 `Result` 时忽略仅用于传播与诊断的 `cause` 元数据。嵌套字段必须自身满足 Eq；含 `Any`、`dyn` 或闭包字段的 Struct 因而不能默认比较，除非提供手写实现。类字段按 Class 身份参与 Struct 相等。

类型可提供同步实例方法 `fn equals(other: Self): Bool` 覆盖该类型的默认 Eq；自定义 Eq 与 `==`/`!=` 使用同一实现。泛型算法可以声明内置结构约束 `T: Eq`；编译器检查并静态绑定该能力，不需要 `implements` 声明。Class 的 `==` / `!=` 始终是身份比较，不可被方法覆盖。

Hash 与 Eq 分开推导，不能仅因某个值支持 `==` 就用于哈希键。标量、String/Bytes、Enum、Option、Result 和只含可哈希字段的 Struct 可获得默认 `hashCode(): Int`；复合值按分支/字段顺序组合哈希，浮点哈希遵守相等值哈希一致（`+0` 与 `-0` 相同）。Class 固定按身份比较，但没有默认 Hash；文件句柄、List、Task、闭包、`Any`、`dyn`、`CPtr` 等引用/动态字段也不会让包含它们的 Struct 自动获得 Hash。字段中的 Class 身份比较仍可用于 Eq，但该 Struct 需要手写 Hash，或在集合构造时提供显式哈希/相等回调。若 Struct 只自定义 `equals`，仍可单独用于相等比较，但不获得默认 Hash；若只自定义 `hashCode`，只有 Hash 能力而不自动保证能与 Eq 组合。一个类型同时满足 `Eq` 与 `Hash`（例如作为默认 HashMap/HashSet 键）时，编译器要求这两个自定义覆盖同时存在，避免自定义 Eq 静默搭配不匹配的结构哈希。相等键必须有相同哈希；修改已存入集合的键所依赖状态会破坏集合查找契约。

### 实例方法与静态方法

- Struct 和 Class 内的 `fn` 默认是实例方法。实例方法及 `init` 的参数列表只写调用者需要传入的参数，接收者不写在签名里。`this` 指当前实例，是不可重新赋值的绑定；Struct 接收者按值传递，Class 接收者保留对象身份。
- 裸名称先查参数和局部绑定，再查实例字段（调用时也查实例方法），最后查模块级名称。用 `this.name` 或 `this.read()` 可显式选中被遮蔽的成员。lambda 不引入新的接收者，可以捕获外层的 `this`；裸成员同样会捕获它。
- `static fn` 属于类型，不接收实例。调用写 `Badge.defaultBadge()`；`pub static fn` 允许跨模块调用。静态方法中没有 `this`，也不自动查找实例成员。静态方法不满足 trait 的实例方法要求。

### 构造与初始化

Struct 按值内联的布局不能成环（包括经 `Option` 或其他内联载荷形成的环）；经 Class 或显式间接引用形成的递归可以。简单主构造写 `class Badge(val title: String)`，`Badge("NEW").title` 是 `"NEW"`。需要计算字段时，可以不写主构造参数列表，改在类里定义一个 `init`：

```lyra
pub class Badge {
    val title: String
    pub init(title: String = "NEW") {
        this.title = if title == "" { "NEW" } else { title }
    }
    pub fn label(): String { title }
    pub static fn defaultBadge(): Badge { Badge() }
}
```

构造调用写 `Badge()` 或 `Badge(title: "Beta")`。`init` 默认模块私有，`pub init` 可以公开构造而不公开字段。构造形参的默认值先于对象创建求值，不能使用 `this`。`init` 正常结束或执行不带值的 `return` 时返回同一个新对象。

Struct 和 Class 的字段都可以写默认表达式，例如 `struct Row(val price: Int, val total: Int = price)`。通过字段构造时，显式实参按调用处从左到右各求值一次，然后按字段声明顺序补齐省略项。`Row(7).total` 得到 `7`。默认表达式在声明模块中求值，可以引用更早的字段和类型形参。每次构造独立求值，因此 `val items: List<T> = []` 每次都创建新列表。跨模块构造时可省略有默认值的私有字段，显式传入的字段须公开。

显式 `init` 在运行用户写的第一条语句前按声明顺序初始化有默认值的字段。它们的默认表达式可引用此前同样有默认值的字段。构造形参和 `this` 可在 `init` 体内使用。已有默认值的 `val` 已经初始化，`var` 仍可改写。需要依赖构造形参或其他待填字段时，在 `init` 体内赋值。

所有字段（包括可空字段）必须在每条正常返回路径上初始化；`val` 字段只能写一次。可直接读已初始化的字段，但调用当前实例的方法、传递或捕获 `this` 要等所有字段就绪。省略 `this.` 不改变这些检查。抛异常而未返回对象的路径无需完成初始化。`if`、`match` 的分支可分别赋值；首版不在可能重复执行或非本地中断的循环、模式守卫、被 `catch` 保护的 `try` 块中首次初始化字段，可先在局部变量计算结果再赋值。每个类只定义一个 `init`；其他创建方式可写静态工厂方法。

## 类型契约

- 首版支持泛型函数、Struct、Class、Enum 和方法；函数/方法约束采用静态结构匹配，约束成员在编译期静态解析。详细边界及代码生成规则见[泛型与接口 RFC](generics-and-interfaces.md)。
- `type UserId = Int` 声明透明类型别名。别名不创建新类型身份、不改变 ABI，支持公开导入与重导出，也可出现在泛型参数中；别名解析形成循环或公开别名暴露私有类型时编译失败。需要与底层类型区分的语义类型仍应使用 Struct。
- 组合与接口用于描述契约和复用。不同具体类型可以先显式转为 `dyn Render`，再经 `List<dyn Render>` 中的接口值动态调用；可写集合不因其元素满足某接口就能整体转换。
- `Any` 是可容纳受管理值的安全装箱顶类型。访问具体成员前须做类型测试或检查转换。保留规范化类型身份（含泛型实参、模块/版本标识）；别名仅作反射/诊断元数据，不产生新身份。这不要求首版提供完整反射 API。
- `val value: Any = item` 按 `item` 的规范类型装箱。`value is T` 检查身份，`value as? T` 返回 `T?`；检查不执行数值或用户转换。`Any` 不声明通用相等性，测试不会就地改变变量的静态类型。`List<T>` 不会整体变成 `List<Any>`；给 `Any...` 展开 `...items` 时，才逐项装箱到该次调用的新列表。

结构约束示例：

```text
trait Render { fn render(): String }
fn draw<T: Render>(item: T): String { item.render() }
class Badge { fn render(): String { "NEW" } }
draw(Badge()) // 结构签名满足约束；无需单独声明实现
```

反例：若 `Badge.render()` 返回 `Int` 而非 `String`，`draw(Badge())` 必须在编译时失败，诊断指出要求的签名与实际签名，不把错误留到运行时。

## 用户自定义隐式转换

**已接受（[答卷 TYPE-07](../design/05-rfc-completion.md) 及本轮复核）：** 除内置的无损数值提升外，用户可以声明源类型到目标类型的隐式转换。返回 `Response` 的函数可以直接写 `return user`；如果函数以 `match` 结尾，同一个预期类型 `Response` 也传给正常返回值的各分支。`async fn` 在这里检查的是完成值，而不是 `Task<Response>`。

例如，[小服务](../examples/user-service/src/main.ly)在定义 `User` 的模块声明默认响应。转换由源类型和目标类型标识，`user` 仅是形参名：

```text
implicit (user: User): Response { Response.json(user) }
```

验收 `LANG-007`：命中 `Ok(Some(user)) => user` 时，这条转换执行一次，结果与本例显式写 `Response.json(user)` 相同；不命中就不执行，404 和 500 仍由 handler 决定。删除这条转换后，即使 `User` 可序列化，也应在分支的 `user` 处报“需要 `Response`，得到 `User`”。能编码 JSON 与选择 HTTP 响应是两个不同的契约。

**已接受：** 源类型或目标类型的定义模块可以声明转换，`pub` 转换随类型可用，不单独导入。[RFC-0003](0003-types.md) 的 `send(user)` 优先选 `send(User)`；只有 `send(Response)` 时，才使用一条可见的直接转换，不串接两次。若两端各有一条同等适用的转换，在实参处报歧义并列出两处声明位置。

固定参数重载按实参逐个比较：精确匹配优于内置无损数值提升，后者优于直接用户转换。剩下的候选若在不同位置各有优势，调用就有歧义，即使其中一个需要转换的次数更少。泛型候选的类型实参确定且满足约束后，按同一规则比较；与非泛型都逐项精确时选非泛型，只有泛型逐项精确时就选泛型。可空提升、整数宽度和变长候选见 [RFC-0003](0003-types.md)。[`?` 适配不同的具体业务错误](errors-and-resources.md#c1-conversions)时也使用普通 `implicit`，不另设错误转换协议。

## 可空类型

- 普通类型默认非空。
- `T?` 与可嵌套的 `Option<T>` 类型等价；`null` 是依照目标可空类型解释的空值构造表达式。
- 有明确目标 `String?` 时可以传入 `String`。对于 `Option<String?>`，`None`、`Some(None)` 和 `Some(Some("Ada"))` 是三个不同的值；对这个类型做穷尽匹配时，漏掉 `Some(None)` 应得到诊断。
- `?.` 安全访问及 `??` 短路空值合并是语言能力；可空与 `Result` 可以组合。
- 类型收窄只用于稳定的局部 `val` 或不重新求值的临时值。Class 属性、可变捕获和共享状态先读成局部快照；跨 `await` 不保留对可变来源的旧检查。
- `if name == null { return 0 }; name.size` 在剩余路径直接访问 `String`，只读形参也适用。`&&` / `||` 的右侧遵循短路路径；若其他仍能到达的路径没有通过检查，则继续把值视为可空。`?.`、`??`、`?` 和与 `null` 的比较仍可显式操作原可空值；局部快照可在 `await` 后继续使用。

后缀 `?` 同时支持 `Result` 和 `Option`；由静态类型决定是传播 `Err`，还是从返回兼容 Optional 的函数提前返回 `None`，两者不隐式互转。嵌套类型可用 `Option<Option<T>>` 明确表达。

## 文本与字节

`String` 是不可变 UTF-8 文本，`Char` 是 Unicode 标量（不等同于用户感知的字素），`Bytes` 是不可变的原始字节。`String.size` 和 `text[index]` 按标量计数，`String.byteSize` 和 `Bytes.size` 按字节计数，`bytes[index]` 得到 `UInt8`。下标从零开始，越界在 `[` 处报错。`Char.codePoint` 为 `UInt32`；可用 `character.toString()` 得到文本。

`text.toBytes()` 保留所有 UTF-8 字节。`bytes.toString()` 完整检查 UTF-8，成功返回 `Some(text)`，非法序列返回 `None`，不做替换或丢弃。`==` 分别比较文本内容、字节内容或字符码点，不进行 Unicode 归一化。`for` 线性遍历文本或字节；文本按标量索引可能从头扫描，不适合用索引逐个遍历长文本。原生字节缓冲区接口另单独设计。

## 封闭枚举与匹配

```text
enum FetchResult { Pending, Ok(status: Int, body: String), Err(String) }

fn display(value: FetchResult): String {
    match value {
        Pending => "加载中",
        Ok(200, body) => body,
        Ok(_, _) => "请求失败",
        Err(message) => message,
    }
}

val pending = FetchResult.Pending
val loaded = FetchResult.Ok(body: "内容", status: 200)
```

- `enum` 至少有一个分支。同一枚举内分支名唯一；每个分支可以不带载荷、带位置载荷或给载荷槽位命名。构造用 `类型名.分支名`；无载荷分支可省括号。命名实参仍按源码从左到右求值，再放入目标槽位。
- 模式按待匹配表达式的类型解析分支，因此自定义的 `Ok` / `Err` 不会被当作 `Result`。也可显式写 `FetchResult.Pending` 或用导入的类型别名限定。匹配时可按位置或按名解构载荷；写 `_` 忽略某个槽位。裸名称与当前枚举的分支同名时先解释为分支；要绑定整个值可换个名称或用反引号。
- `match` 从上到下检查分支；守卫只在结构和字面量已经匹配后执行，带守卫的分支不计入穷尽性检查。穷尽检查会继续检查内部的封闭类型，例如 `Right(true)` 不覆盖同一分支的 `Right(false)`。模式支持可嵌套的 OR（`A | B`）；各备选必须绑定相同名称和类型。整数模式支持包含上界的 `lower..upper` 与不含上界的 `lower..<upper`，两者均参与穷尽性检查；区间端点为整数字面量，倒序或空区间不匹配。未转义的顶层常量名按其常量值比较；使用反引号（`` `Timeout` ``）可强制创建同名绑定。
- 列表模式支持 `[]`、精确长度的 `[first, second]` 和末尾 rest 捕获 `[first, ..rest]`。不带 rest 的模式要求长度完全相等；rest 捕获该位置之后元素组成的浅拷贝 `List<T>`，不会修改原列表槽位，元素引用仍共享。列表按空列表/非空列表递归参与穷尽检查，因此 `[]` 与 `[first, ..rest]` 可覆盖所有列表；固定长度模式单独不能穷尽。
- Struct 和 Class 都可以使用具名字段模式（例如 `User(name: value)`）；字段访问按类型定义模块的可见性规则检查。Struct 字段模式参与递归穷尽性分析。Class 是开放的身份值，即使语言首版没有继承；Class 解构只读取显式列出的字段，不参与穷尽性推断，因此仍需 `_` 或绑定模式覆盖其他对象状态。省略字段等同于 `_`。
- `if let pattern = value { ... } else { ... }` 是单分支模式匹配的表达式写法。`value` 只求值一次，绑定仅在成功分支内可见；省略 `else` 时失败分支为 `Unit`，因此无 `else` 的形式用于 `Unit` 语境。
- `val Point(x: x, y: y) = point` 可以在当前块声明多个只读绑定。右值先求且只求一次；只有能静态证明必匹配的模式才可用于 `val`。对已知 Class 类型可以直接绑定可见字段；带测试的 Class 模式不用于覆盖证明。`val Some(value) = optional` 可能匹配到 `None`，应用 `if let` 处理。普通单名 `val` 始终声明局部名。
- 枚举是值，复制枚举不深拷贝其载荷里引用的对象；分支载荷不能原地改写。递归枚举可直接带本类型作载荷；当前后端把有载荷的分支存入 GC 可扫描的对象，不由此给枚举增加对象身份语义。枚举默认按活动分支和载荷提供 Eq；Hash 还要求所有分支载荷满足 Hash。枚举的 C ABI 布局不公开。
- `pub enum` 可跨模块导入，分支随类型可见，不能作为独立声明导入。枚举的定义模块可像 Struct/Class 的定义模块一样，为枚举声明直接 `implicit` 转换。

## 算术与库表达

- 提供 `Int8/16/32/64`、`UInt8/16/32/64`；`Int`、`UInt` 分别是 64 位别名。普通整数运算在 Debug 和 Release 下均检查溢出；除零也报错，不静默回绕。无符号值不可取负。字面量默认 `Int`，有明确数值目标时直接按目标类型解释；超出目标整数范围在编译时报错。
- `Float` 是 `Float64` 的别名。浮点字面量可写 `3.14`、`2.5e-3`、`3.14f32` 或 `3.14f64`；无后缀时按已知的浮点目标解释，缺少目标则是 `Float64`。浮点运算遵循 IEEE 754，除零可产生无穷大或 `NaN`；`NaN != NaN` 为真，其他与 `NaN` 的比较为假。`%` 为浮点余数。
- 内置无损提升包括同符号整数扩宽、无符号到更宽的有符号整数、`Float32 → Float64`。普通混合整数运算选最窄共同整数类型；普通混合浮点运算选较宽类型。运行时整数值不隐式转浮点，可用 `toFloat()`、`toFloat32()`、`toFloat64()` 显式转换并按目标精度舍入。`toInt8Checked()` / `toUIntChecked()` 等整数目标方法返回 `Option`；越界、非有限浮点或需要丢弃小数时返回 `None`。
- 常用 API 风格为扩展函数、方法链和尾部 lambda。函数值可以传给任意匹配的形参；只有最后一个形参是固定的函数参数时，lambda 才能挪到括号外。期望单个参数时可用 `it`。两层都省略参数名时，`it` 指内层参数；要同时使用外层参数就给它起名。`it` 不引入隐式接收者。
- 扩展方法按接收者静态类型查找，只考虑本模块与显式导入的扩展，同名实例方法优先。同优先级歧义时报错并列出候选；导入顺序不得改变结果。**访问权限按声明扩展的模块判断**：同模块的扩展可以访问接收者的模块私有成员（即使分在不同文件）；外部模块即使导入该类型，也不能用扩展绕过私有边界。
- 扩展方法和只读计算属性放在具名组里，例如 `pub extension ListOps<T> for List<T> { fn first(): T { this[0] }; val head: T get() { this[0] } }`。`pub` 公开组内成员；使用其他模块的组须显式导入组名，也可重导出组。组的类型实参从接收者推断，方法自己的类型实参另行推断。属性 getter 是普通静态检查的合成实例函数，不能异步、不能写入；读取只调用一次 getter，实际字段优先。多个可见扩展为同一接收者提供同名属性时报告歧义。扩展不会为接收者增加字段，也不改变它能满足的结构约束。
- 首版不加管道 `|>`，日常集合操作使用方法链和尾部 lambda；接收者 lambda 留待后续版本。首版运算符优先级、跨行解析和求值顺序见 [RFC-0002](0002-syntax.md)。
