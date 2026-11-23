# std.collections

`map`、`filter` 和 `fold` 是普通 Lyra 泛型函数，没有特殊的编译器规则。按输入列表的顺序处理，空列表不会调用回调。前两者返回新列表；若元素是 class，只复制对象引用。`fold` 在空列表上返回初值。

```lyra
import std.collections.filter
import std.collections.map
import std.collections.fold

fn main(): Unit {
    val selected = filter([1, 2, 3, 4]) { it > 2 }
    val doubled = map(selected) { it * 2 }
    print(fold(doubled, 0, (total: Int, value: Int) => { total + value }))
}
```

程序打印 `14`。尾部块只绑定在固定的最后一个函数形参；`it` 表示这个块的单个参数。也可以按位置传入已有的具名函数。

## 字典与集合

```lyra
import std.collections.stringMap
import std.collections.intSet

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
- `stringSet()`、`intSet()` 返回 `HashSet<Item>`；`add` 和 `remove` 只在集合发生改变时返回 `true`。Map/Set 都以引用共享；`entries()`、`values()` 返回独立列表快照，不承诺顺序。快照不会深拷贝 Class 对象。
- `for entry in map` 得到 `MapEntry<Key,Value>`（含 `key` 和 `value`），`for item in set` 得到集合元素。每次进入循环都取一份浅快照；之后增删元素或替换值，不影响这次遍历。空集合不执行循环体。元素本身为 `None` 时仍会遍历到它。
- 自定义键用 `hashMap<Key,Value>(hash, equal)` 或 `hashSet<Item>(hash, equal)`。相等的键须哈希一致；键存入后不能改变其哈希或相等所依赖的状态。回调需确定且不能在执行中反过来修改当前集合，不同执行域共同写入时应由调用方同步。
