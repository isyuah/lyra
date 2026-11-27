import std.task.{parallelism}
import std.sync.{AtomicInt}

trait IndexedItems {
    type Item
    fn size(): Int
    fn getOrNone(index: Int): Option<Item>
}

fn mapItems<Items: IndexedItems, R>(values: Items, transform: (Items.Item) -> R): List<R> {
    val output: List<R> = []
    for index in 0..<values.size() { output.add(transform(values.getOrNone(index).unwrap())) }
    output
}

fn filterItems<Items: IndexedItems>(values: Items, predicate: (Items.Item) -> Bool): List<Items.Item> {
    val output: List<Items.Item> = []
    for index in 0..<values.size() {
        val value = values.getOrNone(index).unwrap()
        if predicate(value) { output.add(value) }
    }
    output
}

fn foldItems<Items: IndexedItems, R>(values: Items, initial: R, combine: (R, Items.Item) -> R): R {
    var result = initial
    for index in 0..<values.size() { result = combine(result, values.getOrNone(index).unwrap()) }
    result
}

fn anyItems<Items: IndexedItems>(values: Items, predicate: (Items.Item) -> Bool): Bool {
    for index in 0..<values.size() {
        val value = values.getOrNone(index).unwrap()
        if predicate(value) { return true }
    }
    false
}

fn findItem<Items: IndexedItems>(values: Items, predicate: (Items.Item) -> Bool): Option<Items.Item> {
    for index in 0..<values.size() {
        val value = values.getOrNone(index).unwrap()
        if predicate(value) { return Some(value) }
    }
    None
}

struct SortEntry<T>(val key: Int, val item: T)

fn mergeLimit(start: Int, width: Int, count: Int): Int {
    if width >= count - start { count } else { start + width }
}

fn sortedItems<Items: IndexedItems>(values: Items, key: (Items.Item) -> Int): List<Items.Item> {
    var source = mapItems<Items, SortEntry<Items.Item>>(values) { item => SortEntry(key(item), item) }
    val count = source.size
    var target = source.slice(0..<count)
    var width = 1
    while width < count {
        var first = 0
        while first < count {
            val middle = mergeLimit(first, width, count)
            val end = mergeLimit(middle, width, count)
            var left = first
            var right = middle
            for index in first..<end {
                if right == end || (left < middle && source[left].key <= source[right].key) {
                    target[index] = source[left]
                    left += 1
                } else {
                    target[index] = source[right]
                    right += 1
                }
            }
            first = end
        }
        val previous = source
        source = target
        target = previous
        width = mergeLimit(width, width, count)
    }
    mapItems<List<SortEntry<Items.Item>>, Items.Item>(source) { it.item }
}

/// 按输入顺序转换每个元素，返回一个新列表。
pub fn map<T, R>(values: List<T>, transform: (T) -> R): List<R> { mapItems<List<T>, R>(values, transform) }
/// 按输入顺序转换只读列表的每个元素，返回一个可写的新列表。
pub fn map<T, R>(values: FrozenList<T>, transform: (T) -> R): List<R> { mapItems<FrozenList<T>, R>(values, transform) }
/// 筛出满足 `predicate` 的元素，按原顺序放入新列表。
pub fn filter<T>(values: List<T>, predicate: (T) -> Bool): List<T> { filterItems<List<T>>(values, predicate) }
/// 从只读列表筛出匹配元素，按原顺序放入可写的新列表。
pub fn filter<T>(values: FrozenList<T>, predicate: (T) -> Bool): List<T> { filterItems<FrozenList<T>>(values, predicate) }
/// 从 `initial` 开始按输入顺序累积；空列表返回初值，不调用 `combine`。
pub fn fold<T, R>(values: List<T>, initial: R, combine: (R, T) -> R): R { foldItems<List<T>, R>(values, initial, combine) }
/// 从左到右归并只读列表；没有元素时返回 `initial`。
pub fn fold<T, R>(values: FrozenList<T>, initial: R, combine: (R, T) -> R): R { foldItems<FrozenList<T>, R>(values, initial, combine) }
/// 在首个匹配处返回 `true`；没有匹配（包括空列表）时返回 `false`。
pub fn any<T>(values: List<T>, predicate: (T) -> Bool): Bool { anyItems<List<T>>(values, predicate) }
/// 判断只读列表中是否存在匹配项，并在首个匹配处停止。
pub fn any<T>(values: FrozenList<T>, predicate: (T) -> Bool): Bool { anyItems<FrozenList<T>>(values, predicate) }
/// 返回首个匹配项；没有匹配时返回 `None`，匹配到空的可选值时返回 `Some(None)`。
pub fn find<T>(values: List<T>, predicate: (T) -> Bool): Option<T> { findItem<List<T>>(values, predicate) }
/// 返回只读列表中的首个匹配项；没有匹配时返回 `None`。
pub fn find<T>(values: FrozenList<T>, predicate: (T) -> Bool): Option<T> { findItem<FrozenList<T>>(values, predicate) }
/// 按 `Int` 键稳定升序，返回浅拷贝；按输入顺序对每项求键一次，求键时不要修改输入。
pub fn sortedBy<T>(values: List<T>, key: (T) -> Int): List<T> { sortedItems<List<T>>(values, key) }
/// 按 `Int` 键稳定升序，返回可写的浅拷贝；同键元素保持原顺序。
pub fn sortedBy<T>(values: FrozenList<T>, key: (T) -> Int): List<T> { sortedItems<FrozenList<T>>(values, key) }

/// 从左到右对整数求和；空列表返回 `0`，溢出沿用 `Int` 加法规则。
pub fn sum(values: List<Int>): Int {
    var total = 0
    for value in values { total += value }
    total
}

/// 按相等性去重，保留每个值首次出现的位置顺序，返回新列表。
pub fn distinct<T: Eq & Hash>(values: List<T>): List<T> {
    val seen = hashSet<T>()
    val output: List<T> = []
    for value in values {
        if seen.add(value) { output.add(value) }
    }
    output
}

async fn parallelForWorker<T>(values: List<T>, next: AtomicInt, action: (T) -> Task<Unit>): Unit {
    while true {
        val index = next.fetchAdd(1)
        if index >= values.size { return }
        await action(values[index])
    }
}

/// 在并行域中处理列表元素，最多同时执行 `maxConcurrency` 个回调，且不超过工作线程池容量。
/// 上限必须为正。等所有工作域结束才返回；运行期间不得通过任何别名修改输入列表。
pub async fn parallelFor<T>(values: List<T>, maxConcurrency: Int = parallelism(), action: (T) -> Task<Unit>): Unit {
    if maxConcurrency < 1 { throw Exception("parallelFor maxConcurrency must be positive") }
    val workerLimit = parallelism()
    var workers = if maxConcurrency < workerLimit { maxConcurrency } else { workerLimit }
    if workers > values.size { workers = values.size }
    if workers == 0 { return }
    val next = AtomicInt()
    taskGroup {
        for _ in 0..<workers {
            parallel {
                await parallelForWorker(values, next, action)
            }
        }
    }
}

/// 导入后可在可写列表上用方法链调用遍历、筛选和排序操作。
pub extension ListOps<T> for List<T> {
    /// 按顺序转换每个元素，返回新列表。
    fn map<R>(transform: (T) -> R): List<R> { map(this, transform) }
    /// 保留满足条件的元素，保持原顺序并返回新列表。
    fn filter(predicate: (T) -> Bool): List<T> { filter(this, predicate) }
    /// 按原顺序累积，空列表返回初值。
    fn fold<R>(initial: R, combine: (R, T) -> R): R { fold(this, initial, combine) }
    /// 是否存在匹配项；遇到第一项就停止。
    fn any(predicate: (T) -> Bool): Bool { any(this, predicate) }
    /// 返回首个匹配项，找不到时返回 `None`。
    fn find(predicate: (T) -> Bool): Option<T> { find(this, predicate) }
    /// 每项求键一次，返回按 `Int` 键稳定升序的浅拷贝；求键时不要修改列表。
    fn sortedBy(key: (T) -> Int): List<T> { sortedBy(this, key) }
}

/// 导入后可用方法链处理只读列表；产生列表的操作返回可写结果。
pub extension FrozenListOps<T> for FrozenList<T> {
    /// 按顺序转换每个元素，返回新的可写列表。
    fn map<R>(transform: (T) -> R): List<R> { map(this, transform) }
    /// 按原顺序收集匹配项，返回新的可写列表。
    fn filter(predicate: (T) -> Bool): List<T> { filter(this, predicate) }
    /// 按原顺序累积，空列表返回初值。
    fn fold<R>(initial: R, combine: (R, T) -> R): R { fold(this, initial, combine) }
    /// 是否存在匹配项；遇到第一项就停止。
    fn any(predicate: (T) -> Bool): Bool { any(this, predicate) }
    /// 返回首个匹配项，找不到时返回 `None`。
    fn find(predicate: (T) -> Bool): Option<T> { find(this, predicate) }
    /// 返回按 `Int` 键稳定升序的可写浅拷贝；同键项保持原顺序。
    fn sortedBy(key: (T) -> Int): List<T> { sortedBy(this, key) }
}
