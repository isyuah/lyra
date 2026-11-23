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
