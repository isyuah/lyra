# std.collections

`map`、`filter`、`fold`、`any`、`find`、`sortedBy` 同时接受 `List<T>` 和 `FrozenList<T>`，可作为普通泛型函数单独导入。显式导入对应的 `ListOps` 或 `FrozenListOps` 后，也可用方法链调用。

```lyra
import std.collections.{ListOps}

fn main(): Unit {
    val doubled = [1, 2, 3, 4].filter { it > 2 }.map { it * 2 }
    print(doubled.fold(0, (total: Int, value: Int) => { total + value }))
    print(doubled.find { it > 6 } ?? 0)
}
```

程序打印 `14`、`8`。操作按输入列表的顺序处理，空列表不调用回调。`map`、`filter` 返回新列表；若元素是 Class，只复制对象引用。空列表的 `fold` 返回初值。`any` 和 `find` 在首个匹配处停止；无匹配分别返回 `false`、`None`，而匹配到可空元素 `None` 时，`find` 返回 `Some(None)`。单参尾部块里的 `it` 是当前元素，也可以传入已有的具名函数。

`items.slice(range)` 是内置的 `List<T>` 方法，无需导入。`1..<3` 取下标 `1`、`2`，`1..3` 还包含下标 `3`。切片返回独立浅拷贝：替换新列表里的元素不会改写原列表，但 Class 元素仍是同一个对象。半开空切片可从列表末端开始；负数下标、倒序和超出列表边界与直接索引一样会报错并定位到调用处。

两种列表都内置 `getOrNone(index)`、`firstOrNone()` 和 `lastOrNone()`；越界或没有元素时返回 `None`，读到空元素时返回 `Some(None)`。`FrozenList.slice(range)` 返回只读切片，`map`、`filter`、`sortedBy` 返回新的可写列表。两种列表都以 `Item` 公开元素关联类型，供结构约束使用。

- `sortedBy(values, key)` 或 `values.sortedBy { ... }` 按 `Int` 键稳定升序，按输入顺序对每项求键一次。同键元素保留原顺序，输出是独立的浅拷贝；求键时不要修改输入列表。
- `sum(values: List<Int>)` 从左到右求和，空列表返回 `0`，加法沿用普通 `Int` 溢出规则。
- `distinct<T: Eq & Hash>(values)` 返回独立列表，每个相等类保留首次出现的元素，也保留首次出现的顺序。

## 有界并行遍历

`parallelFor(values, maxConcurrency) { item => ... }` 返回冷 `Task<Unit>`；首次 `await` 或 `spawn` 才启动，完成前等待任务组里的并行域收尾。回调形参是 `T -> Task<Unit>`，因此可在回调中 `await` I/O 或其他异步工作。并发上限取传入值与 `std.task.parallelism()` 的较小值，缺省使用 Worker 池容量；超出列表长度时按元素数裁剪。并行域通过共享的原子游标领取下一项，每个域一次等待一个回调，不为每个元素启动线程或并行域。显式传入的并发上限必须大于零；空列表无需启动 Worker。

```lyra
import std.collections.{parallelFor}

async fn process(item: Int): Unit { await blocking { writeRecord(item) } }
async fn main(): Unit { await parallelFor([1, 2, 3, 4]) { process(it) } }
```

回调可能在不同 OS 线程同时执行；共享可变对象仍由调用方用锁或原子操作保护。`parallelFor` 不会复制输入列表：从 Task 首次启动到完成期间，调用方和回调都不得经任何别名修改它（包括追加、删除或替换元素），也不得让其他执行域同时修改。此期间的列表修改不受支持；需要更新数据时，请在回调中写入独立结果，等待遍历完成后再合并。失败会使任务组按结构化并发规则取消其他并行域并等待其清理；显式取消返回的 Task 也会传播到工作域，并等待其清理。

`FrozenList(items)` 创建列表槽位的只读浅快照，可索引和遍历。此后原列表增删、重排或替换槽位不会改变快照；元素是 Class 时仍共享同一个对象。用它跨域发布一批稳定的数据，需要对对象内部做复合更新时仍应同步。

## 字典与集合

```lyra
import std.collections.{stringMap}
import std.collections.{intSet}

fn main(): Unit {
    val scores = stringMap<Int>()
    scores.put("Ada", 42)
    for entry in scores { print("${entry.key}: ${entry.value}") }

    val seen = intSet()
    seen.add(7)
    for item in seen { print(item) }
}
```

- `stringMap<Value>()`、`intMap<Value>()` 返回 `HashMap<Key,Value>`。`get`、`put`、`remove` 分别返回可空的当前值或旧值；`None` 表示没有此键。若值本身是可空类型，`Some(None)` 表示键存在、值为空。`contains` 和 `size()` 不用取出值。
- `stringSet()`、`intSet()` 返回 `HashSet<Item>`；`add` 和 `remove` 只在集合发生改变时返回 `true`。Map/Set 都以引用共享；`entries()`、`keys()`、`values()` 返回独立列表快照，不承诺顺序。快照不会深拷贝 Class 对象。
- `hashMap<Key: Eq & Hash,Value>()` 和 `hashSet<Item: Eq & Hash>()` 使用类型的 `==` 与 `hashCode()`；Struct、Enum、Option、Result 可按字段或活动载荷自动支持这两种能力。Class 按身份比较但不默认 Hash；Struct 含闭包、Class/File 句柄、List、Task、`Any`、`dyn` 等不能默认 Hash 的字段时，可用手写成对的 `equals` / `hashCode` 方法或显式回调。
- `for entry in map` 得到 `MapEntry<Key,Value>`（含 `key` 和 `value`），`for item in set` 得到集合元素。每次进入循环都取一份浅快照；之后增删元素或替换值，不影响这次遍历。空集合不执行循环体。元素本身为 `None` 时仍会遍历到它。
- 自定义键用 `hashMap<Key,Value>(hash, equal)` 或 `hashSet<Item>(hash, equal)` 覆盖默认能力。相等的键须哈希一致；键存入后不能改变其哈希或相等所依赖的状态。回调需确定且不能在执行中反过来修改当前集合，不同执行域共同写入时应由调用方同步。
