# Lyra RFC：泛型与接口

**状态：首版泛型范围、静态结构约束、显式 `dyn` 接口值及可变集合不变已接受；装箱布局和高级约束另议**  
**范围：** 泛型约束怎样判断、受约束成员怎样解析、代码生成策略与语言行为的边界。

## 已接受的语义

- 泛型约束使用静态结构匹配。无需单独声明 `implements`；类型只要拥有约束要求的可见成员及匹配签名，就可用于该泛型。
- 首版覆盖泛型函数、Struct、Class、Enum、集合和 `Task<T>`。`trait` 和 `T: Render` 用于表达算法需要哪些成员，不用来证明普通对象可复制或可跨域传递。
- 泛型函数中的约束成员在编译期解析。普通泛型调用不在运行时搜索任意同名成员。
- 编译器可选择单态化、共享机器码、字典传递或组合策略，但策略不得改变类型检查结果、类型/值语义或所调用的方法。
- 组合与接口用于代码复用。带状态的 Class 单继承不支持。

最小的泛型函数不需要 trait，也不需要装饰器：

```text
fn identity<T>(value: T): T { value }

val inferred = identity(7)
val explicit = identity<Int>(7)
```

`identity` 对每个 `T` 都原样返回传入值。第一行调用从实参推得 `T = Int`，第二行显式指定 `Int`，两者都返回 `7`。明确的预期结果类型也能提供推断线索；信息不足时在调用处指出缺少哪个类型参数，不默认取 `Any`。见 [RFC-0002](0002-syntax.md) 和[推断正反例](0002-syntax-cases.md)。

若实参已直接确定 `T`，外层函数要求的用户转换只作用于返回的结果。显式指定 `T` 则可能要求在传参时先转换。两者的可观察顺序，以及类型实参确定且约束满足后的重载排序，见 [RFC-0003](0003-types.md)。

只有函数要使用特定成员时才需要约束，例如：

```text
trait Render { fn render(): String }

fn draw<T: Render>(item: T): String {
    item.render()
}

class Badge {
    fn render(): String { "NEW" }
}

draw(Badge()) // 成功：Badge 的方法签名满足 Render
```

若 `Badge.render()` 返回 `Int` 而约束要求 `String`，程序必须编译失败，诊断指出实际签名与要求的差异；不能把不匹配留到运行时。

**Alpha 可用范围。** 一个类型形参可声明一个命名的结构约束；同一个 `trait` 可列出多个实例方法。签名包含方法名、具名形参及类型、结果和 `async`，形参名参与匹配，因为通过约束也可以按名称调用。方法须在**泛型定义所在的模块**可见。定义检查只允许调用约束中列出的方法；实例化时检查具体类型并直接生成对该实现的调用，不做运行时反射。约束可以跨包导入或在泛型之间转发，未调用的泛型函数体同样检查。这里不要求类型声明 `implements`，也不为普通对象增加所有权或线程资格标记。泛型方法签名作为约束要求、多个独立 bound 和 `dyn` 接口值留给后续版本。

## 泛型与动态接口的边界

泛型 Struct 和 Class 与泛型函数共用类型实参语法：

```lyra
struct Pair<First,Second>(val first: First, val second: Second)

class Cell<Item>(var value: Item, var next: Cell<Item>?) {
    static fn single(value: Item): Cell<Item> { Cell<Item>(value, None) }
    fn map<Other>(transform: (Item) -> Other): Cell<Other> {
        Cell<Other>.single(transform(value))
    }
}

val cell = Cell<String>.single("Ada")
val length = cell.map { it.size }
```

构造可以显式写 `Cell<String>(...)`，也可从实参和明确的结果目标推断。编译器先收集普通实参的类型，再用它们检查需要上下文的 `None`、空列表和 lambda；运行时仍按源码顺序求值。调用实例方法时，`Item` 固定为接收者的类型实参，方法自己的 `Other` 另行推断或显式指定。类型检查会检查尚未实例化的泛型方法；原生代码只使用具体布局。

静态方法没有实例来提供类型实参。`Cell<String>.single("Ada")` 固定了 `Item`；`Cell.single("Ada")` 可从实参或明确的结果类型推断。若要显式指定静态方法自己的类型实参，先写齐所属类型的实参，例如 `Box<Int>.from<String>(...)`。

`Pair<Int,String>` 和 `Pair<String,Int>` 是不同类型；跨模块导入别名仍指向同一个类型定义。匹配 `Pair(first: value)` 时，从被匹配值确定类型实参。Class 引用可形成递归；Struct 的内联布局不能成环。泛型声明的宏派生留在后续切片。

类型声明可为每个类型形参声明一个结构约束，例如 `struct Card<T: Render>(val item: T)`。约束同时作用于类型标注、构造（含 `init`）、静态和实例方法、字段默认值以及枚举分支。方法还可对自己的类型形参另加约束。查找满足约束的方法时使用声明约束的模块的可见性，不由调用点临时引入成员。

`enum Load<T> { Pending, Ready(T), Failed(String) }` 的每个实例共享分支集合，载荷按类型实参具体化。`Load.Ready(42)` 从载荷推断 `T`；`Load.Pending` 需要明确的目标类型，否则写 `Load<Int>.Pending`。模式仍写 `Load.Pending` 或 `Ready(value)`，由被匹配值决定类型实参；跨包导入别名不产生新的枚举身份。

结构约束解决“这个具体类型能否传给某个泛型函数”。它本身不解决“如何在同一集合中保存不同具体类型，然后通过共同接口动态调用”。

```text
fn draw<T: Render>(item: T) { ... }  // 每次调用有一个具体 T
val items: List<dyn Render>         // 元素可来自不同具体类型
```

异构集合用显式接口值 `dyn Render`。把具体值显式转成该接口值时，编译器检查可见成员的签名；经接口值调用按实际类型分派。可写的 `List<Badge>` 不能当作 `List<dyn Render>`，否则另一个调用者可以向原列表写入非 `Badge` 的值。可逐个转换元素，构造另一份异构列表。这个约束用于可写集合，与对象能否跨域共享无关。

## 开放事项

- 多重约束、成员约束及显式接口对象转换的表达方式。
- 可写集合以外的变型、递归约束、接口值的装箱布局和默认实现冲突。
- 泛型跨包编译与缓存、代码膨胀控制。关联类型与用户指定特化不列入首版。
- 稳定 C ABI 不直接承诺泛型签名；具体 C 导出应使用具象的包装接口，ABI 规则另行定义。

## 一致性要求

- 结构匹配成功的具体类型可传给受约束泛型，不要求手写实现声明。
- 缺少所需成员或成员签名不匹配时，编译器拒绝调用并定位到约束/实参。
- 不同泛型代码生成策略不得改变同一程序的结果或诊断类别。
- `List<Badge>` 传给固定的 `List<dyn Render>` 形参时报类型不匹配；已经转换成 `dyn Render` 的值可同置一个列表，按各自实现调用 `render()`。
