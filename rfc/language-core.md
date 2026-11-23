# Lyra RFC：语言核心与类型

**状态：已接受核心设计方向；具体语法和部分类型规则仍开放**  
**范围：** 表达式与函数、绑定可变性、对象模型、类型契约、空值和基本算术。调用、词法与控制流的详细草案见 [RFC-0002](0002-syntax.md)；函数装饰器及数据序列化见[元编程与数据 RFC](metaprogramming-data.md)。

## 表达式与函数

- 分号可省。块正常结束时，末尾表达式的结果就是块值；加 `;` 则丢弃结果，块值为 `Unit`。`return expr;` 仍显式返回 `expr` 的值。`if` 在值位置必须有 `else`；`match` 对封闭枚举和 `Option` 必须穷尽，普通循环返回 `Unit`。
- 具名函数的参数类型显式声明。局部变量、lambda 和私有非递归函数的返回类型可按上下文推导；公开函数和递归函数必须声明返回类型。
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

`mark("R")` 必须先于 `mark("L")` 求值，前者的结果绑定到 `right`。位置实参在命名实参之前，每个固定形参最多绑定一次。缺省的默认表达式按形参声明顺序求值，只能引用更早的形参。

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

可变 `List` 赋值共享同一个集合；经一个别名修改元素，其他别名能看到。闭包捕获绑定本身，不暗中复制当前值；首版 `return` 不从调用该闭包的外层函数返回。Class 默认按对象身份相等，这里的身份不是可观察的固定地址；Struct 未实现 `Eq` 则不能用 `==` 比较。

Struct 按值内联的布局不能成环（包括经 `Option` 或其他内联载荷形成的环）；经 Class 或显式间接引用形成的递归可以。简单主构造写 `class Badge(val title: String)`，`Badge("NEW").title` 是 `"NEW"`；复杂初始化可写显式构造函数。Class 允许多个构造函数、私有构造函数和静态工厂；所有非空字段就绪前 `this` 不得逃逸。

## 类型契约

- 首版支持泛型函数、Struct、Class 和方法；函数/方法约束采用静态结构匹配，约束成员在编译期静态解析。详细边界及代码生成规则见[泛型与接口 RFC](generics-and-interfaces.md)。
- 组合与接口用于描述契约和复用。不同具体类型可以先显式转为 `dyn Render`，再经 `List<dyn Render>` 中的接口值动态调用；可写集合不因其元素满足某接口就能整体转换。
- `Any` 是可容纳受管理值的安全装箱顶类型。访问具体成员前须做类型测试或检查转换。保留规范化类型身份（含泛型实参、模块/版本标识）；别名仅作反射/诊断元数据，不产生新身份。这不要求首版提供完整反射 API。

结构约束示例：

```text
trait Render { fn render(self): String }
fn draw<T: Render>(item: T): String { item.render() }
class Badge { fn render(self): String { "NEW" } }
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
- `match` 从上到下检查分支；守卫只在结构和字面量已经匹配后执行，带守卫的分支不计入穷尽性检查。穷尽检查会继续检查内部的封闭类型，例如 `Right(true)` 不覆盖同一分支的 `Right(false)`。
- 枚举是值，复制枚举不深拷贝其载荷里引用的对象；分支载荷不能原地改写。递归枚举可直接带本类型作载荷；当前后端把有载荷的分支存入 GC 可扫描的对象，不由此给枚举增加对象身份语义。首版不为枚举默认提供 `==`，也不暴露枚举的 C ABI 布局。
- `pub enum` 可跨模块导入，分支随类型可见，不能作为独立声明导入。枚举的定义模块可像 Struct/Class 的定义模块一样，为枚举声明直接 `implicit` 转换。

## 算术与库表达

- 提供 `Int8/16/32/64`、`UInt8/16/32/64`；`Int`、`UInt` 分别是 64 位别名。普通整数运算在 Debug 和 Release 下均检查溢出；除零也报错，不静默回绕。无符号值不可取负。字面量默认 `Int`，有明确数值目标时直接按目标类型解释；超出目标整数范围在编译时报错。
- `Float` 是 `Float64` 的别名。浮点字面量可写 `3.14`、`2.5e-3`、`3.14f32` 或 `3.14f64`；无后缀时按已知的浮点目标解释，缺少目标则是 `Float64`。浮点运算遵循 IEEE 754，除零可产生无穷大或 `NaN`；`NaN != NaN` 为真，其他与 `NaN` 的比较为假。`%` 为浮点余数。
- 内置无损提升包括同符号整数扩宽、无符号到更宽的有符号整数、`Float32 → Float64`。普通混合整数运算选最窄共同整数类型；普通混合浮点运算选较宽类型。运行时整数值不隐式转浮点，可用 `toFloat()`、`toFloat32()`、`toFloat64()` 显式转换并按目标精度舍入。`toInt8Checked()` / `toUIntChecked()` 等整数目标方法返回 `Option`；越界、非有限浮点或需要丢弃小数时返回 `None`。
- 常用 API 风格为扩展函数、方法链和尾部 lambda。函数值可以传给任意匹配的形参；只有最后一个形参是固定的函数参数时，lambda 才能挪到括号外。期望单个参数时可用 `it`。两层都省略参数名时，`it` 指内层参数；要同时使用外层参数就给它起名。`it` 不引入隐式接收者。
- 扩展方法按接收者静态类型查找，只考虑本模块与显式导入的扩展，同名实例方法优先。同优先级歧义时报错并列出候选；导入顺序不得改变结果。**访问权限按声明扩展的模块判断**：同模块的扩展可以访问接收者的模块私有成员（即使分在不同文件）；外部模块即使导入该类型，也不能用扩展绕过私有边界。
- 首版不加管道 `|>`，日常集合操作使用方法链和尾部 lambda。接收者 lambda、扩展属性仍开放；首版运算符优先级、跨行解析和求值顺序见 [RFC-0002](0002-syntax.md)。
