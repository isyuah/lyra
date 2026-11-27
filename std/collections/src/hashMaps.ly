/// 映射快照中的一对键和值。
pub struct MapEntry<Key,Value>(
    /// 快照取样时存储的键。
    pub val key: Key,
    /// 快照取样时与键关联的值。
    pub val value: Value
)

/// 遍历 Map 或 Set 的浅快照，原集合的增删不会改变已取的快照。
pub class SnapshotIterator<Item>(val snapshot: List<Item>, var position: Int = 0) {
    /// 取下一个元素；耗尽返回 `None`，可选的空元素仍包装成 `Some(None)`。
    pub fn next(): Option<Item> {
        if position == snapshot.size { return None }
        val item = snapshot[position]
        position += 1
        Some(item)
    }
}

class HashNode<Key,Value>(val key: Key, val hashCode: Int, var value: Value, var next: HashNode<Key,Value>?)

fn emptyBuckets<Key,Value>(capacity: Int): List<HashNode<Key,Value>?> {
    val buckets: List<HashNode<Key,Value>?> = []
    for index in 0..<capacity { buckets.add(None) }
    buckets
}

fn bucketFor(hashCode: Int, capacity: Int): Int {
    val remainder = hashCode % capacity
    if remainder < 0 { remainder + capacity } else { remainder }
}

/// 可变哈希映射，不保证遍历顺序；跨执行域共享写入需要调用方同步。
pub class HashMap<Key,Value>(val keyHash: (Key) -> Int, val keysEqual: (Key, Key) -> Bool, var buckets: List<HashNode<Key,Value>?>, var count: Int) {
    /// 当前键值对的数量。
    pub fn size(): Int { count }

    fn find(key: Key, hashCode: Int): HashNode<Key,Value>? {
        val index = bucketFor(hashCode, buckets.size)
        var current = buckets[index]
        while current != None {
            val node = current?
            if node.hashCode == hashCode && keysEqual(node.key, key) { return Some(node) }
            current = node.next
        }
        None
    }

    /// 查找键的当前值；键缺失返回 `None`，键存在而值为空时返回 `Some(None)`。
    pub fn get(key: Key): Value? {
        val node = find(key, keyHash(key))?
        Some(node.value)
    }

    /// 判断键是否存在，不要求取出或比较对应的值。
    pub fn contains(key: Key): Bool { find(key, keyHash(key)) != None }

    fn grow(): Unit {
        val previous = buckets
        val larger = emptyBuckets<Key,Value>(previous.size * 2)
        for head in previous {
            var current = head
            while true {
                match current {
                    None => { break },
                    Some(node) => {
                        current = node.next
                        val index = bucketFor(node.hashCode, larger.size)
                        node.next = larger[index]
                        larger[index] = Some(node)
                    },
                }
            }
        }
        buckets = larger
    }

    /// 插入或替换键的值，返回替换前的值；新键返回 `None`。
    pub fn put(key: Key, value: Value): Value? {
        val hashCode = keyHash(key)
        match find(key, hashCode) {
            Some(node) => { val previous = node.value; node.value = value; return Some(previous) },
            None => {},
        }
        if count == buckets.size { grow() }
        val index = bucketFor(hashCode, buckets.size)
        buckets[index] = Some(HashNode(key, hashCode, value, buckets[index]))
        count += 1
        None
    }

    /// 删除键并返回它原有的值；键未出现时返回 `None`。
    pub fn remove(key: Key): Value? {
        val hashCode = keyHash(key)
        val index = bucketFor(hashCode, buckets.size)
        var previous: HashNode<Key,Value>? = None
        var current = buckets[index]
        while current != None {
            val node = current?
            if node.hashCode == hashCode && keysEqual(node.key, key) {
                match previous {
                    None => { buckets[index] = node.next },
                    Some(parent) => { parent.next = node.next },
                }
                count -= 1
                return Some(node.value)
            }
            previous = Some(node)
            current = node.next
        }
        None
    }

    fn snapshot<Output>(project: (Key, Value) -> Output): List<Output> {
        val output: List<Output> = []
        for head in buckets {
            var current = head
            while true {
                match current {
                    None => { break },
                    Some(node) => {
                        output.add(project(node.key, node.value))
                        current = node.next
                    },
                }
            }
        }
        output
    }

    /// 取键值对的浅快照，不保证列表顺序。
    pub fn entries(): List<MapEntry<Key,Value>> { snapshot((key, value) => { MapEntry(key, value) }) }
    /// 取键的独立浅拷贝列表，不保证顺序。
    pub fn keys(): List<Key> { snapshot((key, _) => { key }) }
    /// 每次调用都为当前键值对创建新的快照迭代器。
    pub fn iterator(): SnapshotIterator<MapEntry<Key,Value>> { SnapshotIterator(entries()) }
}

/// 使用指定哈希和相等规则创建空映射。相等的键哈希值必须一致，入表后不要改变键的比较结果。
pub fn hashMap<Key,Value>(hash: (Key) -> Int, equal: (Key, Key) -> Bool): HashMap<Key,Value> {
    HashMap(hash, equal, emptyBuckets<Key,Value>(8), 0)
}

/// 用键类型的 `hashCode()` 和相等性创建空映射。
pub fn hashMap<Key: Eq & Hash,Value>(): HashMap<Key,Value> {
    hashMap<Key,Value>((key) => { key.hashCode() }, (left, right) => { left == right })
}

fn hashString(key: String): Int {
    var result = 0
    for byte in key.toBytes() { result = (result * 131 + byte) % 2147483647 }
    result
}

/// 创建以区分大小写的字符串为键的空映射。
pub fn stringMap<Value>(): HashMap<String,Value> {
    hashMap<String,Value>(hashString, (left, right) => { left == right })
}

/// 创建按数值识别 `Int` 键的空映射。
pub fn intMap<Value>(): HashMap<Int,Value> {
    hashMap<Int,Value>((key) => { key }, (left, right) => { left == right })
}

/// 按哈希与相等性维护唯一元素的可变集合，不保证遍历顺序。
pub class HashSet<Item>(val items: HashMap<Item,Unit>) {
    /// 当前不同元素的数量。
    pub fn size(): Int { items.size() }
    /// 判断集合中是否有与 `item` 相等的元素。
    pub fn contains(item: Item): Bool { items.contains(item) }
    /// 加入一个元素；只在集合改变时返回 `true`。
    pub fn add(item: Item): Bool { items.put(item, {}) == None }
    /// 删除匹配项；只在确实删除时返回 `true`。
    pub fn remove(item: Item): Bool { items.remove(item) != None }
    /// 返回当前元素的独立浅拷贝列表，不保证顺序。
    pub fn values(): List<Item> { items.keys() }
    /// 为调用时的集合内容创建快照迭代器。
    pub fn iterator(): SnapshotIterator<Item> { SnapshotIterator(values()) }
}

/// 用指定哈希与相等规则创建空集合；相等元素必须有相同哈希，入集合后规则所依赖的状态不得变化。
pub fn hashSet<Item>(hash: (Item) -> Int, equal: (Item, Item) -> Bool): HashSet<Item> { HashSet(hashMap<Item,Unit>(hash, equal)) }
/// 用元素类型的 `hashCode()` 和相等性创建空集合。
pub fn hashSet<Item: Eq & Hash>(): HashSet<Item> { HashSet(hashMap<Item,Unit>()) }
/// 创建按区分大小写的字符串相等性去重的空集合。
pub fn stringSet(): HashSet<String> { HashSet(stringMap<Unit>()) }
/// 创建按 `Int` 数值去重的空集合。
pub fn intSet(): HashSet<Int> { HashSet(intMap<Unit>()) }
