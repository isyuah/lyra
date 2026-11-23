pub struct MapEntry<Key,Value>(pub val key: Key, pub val value: Value)

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
    pub fn size(self): Int { self.count }

    fn find(self, key: Key, hashCode: Int): HashNode<Key,Value>? {
        val index = bucketFor(hashCode, self.buckets.size)
        var current = self.buckets[index]
        while current != None {
            val node = current?
            if node.hashCode == hashCode && self.keysEqual(node.key, key) { return Some(node) }
            current = node.next
        }
        None
    }

    pub fn get(self, key: Key): Value? {
        val node = self.find(key, self.keyHash(key))?
        Some(node.value)
    }

    pub fn contains(self, key: Key): Bool { self.find(key, self.keyHash(key)) != None }

    fn grow(self): Unit {
        val previous = self.buckets
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
        self.buckets = larger
    }

    pub fn put(self, key: Key, value: Value): Value? {
        val hashCode = self.keyHash(key)
        match self.find(key, hashCode) {
            Some(node) => { val previous = node.value; node.value = value; return Some(previous) },
            None => {},
        }
        if self.count == self.buckets.size { self.grow() }
        val index = bucketFor(hashCode, self.buckets.size)
        self.buckets[index] = Some(HashNode(key, hashCode, value, self.buckets[index]))
        self.count += 1
        None
    }

    pub fn remove(self, key: Key): Value? {
        val hashCode = self.keyHash(key)
        val index = bucketFor(hashCode, self.buckets.size)
        var previous: HashNode<Key,Value>? = None
        var current = self.buckets[index]
        while current != None {
            val node = current?
            if node.hashCode == hashCode && self.keysEqual(node.key, key) {
                match previous {
                    None => { self.buckets[index] = node.next },
                    Some(parent) => { parent.next = node.next },
                }
                self.count -= 1
                return Some(node.value)
            }
            previous = Some(node)
            current = node.next
        }
        None
    }

    pub fn entries(self): List<MapEntry<Key,Value>> {
        val snapshot: List<MapEntry<Key,Value>> = []
        for head in self.buckets {
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
}

pub fn hashMap<Key,Value>(hash: (Key) -> Int, equal: (Key, Key) -> Bool): HashMap<Key,Value> {
    HashMap(hash, equal, emptyBuckets<Key,Value>(8), 0)
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
    pub fn size(self): Int { self.items.size() }
    pub fn contains(self, item: Item): Bool { self.items.contains(item) }
    pub fn add(self, item: Item): Bool { self.items.put(item, {}) == None }
    pub fn remove(self, item: Item): Bool { self.items.remove(item) != None }
    pub fn values(self): List<Item> { map(self.items.entries()) { it.key } }
}

pub fn hashSet<Item>(hash: (Item) -> Int, equal: (Item, Item) -> Bool): HashSet<Item> { HashSet(hashMap<Item,Unit>(hash, equal)) }
pub fn stringSet(): HashSet<String> { HashSet(stringMap<Unit>()) }
pub fn intSet(): HashSet<Int> { HashSet(intMap<Unit>()) }
