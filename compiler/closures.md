# 函数值、闭包与尾部 lambda

函数类型 `(Int) -> Int` 表示接收一个 `Int`、返回一个 `Int`。具名函数可以直接用作值；lambda 可以返回、存入字段或列表，也可以在创建它的函数退出后再调用。

```lyra
fn makeCounter(start: Int): (Int) -> Int {
    var total = start
    (delta: Int) => { total += delta; total }
}
fn apply(value: Int, transform: (Int) -> Int): Int { transform(value) }
fn double(value: Int): Int { value * 2 }
fn main(): Unit {
    val count = makeCounter(10)
    print(apply(3, count))                  // 13
    print(apply(value: 2) { count(it) })    // 15
    print(apply(4, double))                 // 8
}
```

- **只省能推断的部分。** `val inc = (n: Int) => { n + 1 }` 自带形参类型；`val inc: (Int) -> Int = (n) => { n + 1 }` 从目标推断。泛型函数作值时也需要目标类型，如 `val id: (Int) -> Int = identity`。
- **尾块只绑定固定的最后一个函数形参。** 零参块不产生 `it`；单参块中的 `it` 是这个参数，不是接收者。嵌套时引用最近一层的隐式 `it`，要同时使用内外层就把外层参数显式命名。多参回调写在括号内，如 `combine((left, right) => { left + right })`。`{` 须和调用结尾同一行；`if ready() { ... }` 的块仍是 `if` 的分支。
- **捕获的是绑定，不是快照。** 同一 `var` 的多个闭包共享更改；循环每次新声明的绑定彼此独立。返回、跨 `await` 或主动 GC 都不会改变被捕获值的生存期。闭包可以交给 `blocking` 执行，但共享可变状态的复合操作不是原子事务。
- **退出不穿透闭包。** `return` 和 `?` 的目标是当前 lambda；该块登记的 `defer` 先逆序执行。`?` 需要已知的 `Option` 或 `Result` 返回类型，可显式写 `(): Result<Int,String> => { ... }`。普通异常可沿调用传播。lambda 本身同步；具名 `async fn` 则可以作为 `(Int) -> Task<Int>` 的值，其间接调用同样只创建冷任务。
- **函数值保留类型，不保留声明的调用简写。** 间接调用按位置传满所有形参；形参名和默认值只用于直接声明调用。变长形参在函数值签名中是一个 `List<T>`。C ABI 不把 Lyra 闭包当作裸函数指针传递。

LLVM 将闭包表示为「代码入口 + 捕获绑定的单元指针」。无捕获的函数值可以使用静态记录；带捕获的记录及绑定单元由 GC 扫描。普通、异步和 `extern fn` 的具名函数通过适配入口复用各自原生调用约定；调用方先计算函数值，再从左到右计算实参。详见[语法规范](../rfc/0002-syntax.md)与[原生验收](../tests/closures_cli.py)。
