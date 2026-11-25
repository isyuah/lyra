pub struct MapEntry<Key,Value>(pub val key: Key, pub val value: Value)

pub class SnapshotIterator<Item>(val snapshot: List<Item>, var position: Int = 0) {
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

pub class HashMap<Key,Value>(val keyHash: (Key) -> Int, val keysEqual: (Key, Key) -> Bool, var buckets: List<HashNode<Key,Value>?>, var count: Int) {
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

    pub fn get(key: Key): Value? {
        val node = find(key, keyHash(key))?
        Some(node.value)
    }

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

    pub fn entries(): List<MapEntry<Key,Value>> {
        val snapshot: List<MapEntry<Key,Value>> = []
        for head in buckets {
            var current = head
            while true {
                match current {
                    None => { break },
                    Some(node) => {
                        snapshot.add(MapEntry(node.key, node.value))
                        current = node.next
                    },
                }
            }
        }
        snapshot
    }

    pub fn iterator(): SnapshotIterator<MapEntry<Key,Value>> { SnapshotIterator(entries()) }
}

pub fn hashMap<Key,Value>(hash: (Key) -> Int, equal: (Key, Key) -> Bool): HashMap<Key,Value> {
    HashMap(hash, equal, emptyBuckets<Key,Value>(8), 0)
}

pub fn hashMap<Key: Eq & Hash,Value>(): HashMap<Key,Value> {
    hashMap<Key,Value>((key) => { key.hashCode() }, (left, right) => { left == right })
}

fn hashString(key: String): Int {
    var result = 0
    for byte in key.toBytes() { result = (result * 131 + byte) % 2147483647 }
    result
}

pub fn stringMap<Value>(): HashMap<String,Value> {
    hashMap<String,Value>(hashString, (left, right) => { left == right })
}

pub fn intMap<Value>(): HashMap<Int,Value> {
    hashMap<Int,Value>((key) => { key }, (left, right) => { left == right })
}

pub class HashSet<Item>(val items: HashMap<Item,Unit>) {
    pub fn size(): Int { items.size() }
    pub fn contains(item: Item): Bool { items.contains(item) }
    pub fn add(item: Item): Bool { items.put(item, {}) == None }
    pub fn remove(item: Item): Bool { items.remove(item) != None }
    pub fn values(): List<Item> { map(items.entries()) { it.key } }
    pub fn iterator(): SnapshotIterator<Item> { SnapshotIterator(values()) }
}

pub fn hashSet<Item>(hash: (Item) -> Int, equal: (Item, Item) -> Bool): HashSet<Item> { HashSet(hashMap<Item,Unit>(hash, equal)) }
pub fn hashSet<Item: Eq & Hash>(): HashSet<Item> { HashSet(hashMap<Item,Unit>()) }
pub fn stringSet(): HashSet<String> { HashSet(stringMap<Unit>()) }
pub fn intSet(): HashSet<Int> { HashSet(intMap<Unit>()) }
