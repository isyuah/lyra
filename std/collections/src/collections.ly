import std.task.parallelism
import std.sync.AtomicInt

pub fn map<T, R>(values: List<T>, transform: (T) -> R): List<R> {
    val output: List<R> = []
    for value in values { output.add(transform(value)) }
    output
}

pub fn filter<T>(values: List<T>, predicate: (T) -> Bool): List<T> {
    val output: List<T> = []
    for value in values {
        if predicate(value) { output.add(value) }
    }
    output
}

pub fn fold<T, R>(values: List<T>, initial: R, combine: (R, T) -> R): R {
    var result = initial
    for value in values { result = combine(result, value) }
    result
}

pub fn any<T>(values: List<T>, predicate: (T) -> Bool): Bool {
    for value in values {
        if predicate(value) { return true }
    }
    false
}

pub fn find<T>(values: List<T>, predicate: (T) -> Bool): Option<T> {
    for value in values {
        if predicate(value) { return Some(value) }
    }
    None
}

struct SortEntry<T>(val key: Int, val item: T)

fn mergeLimit(start: Int, width: Int, count: Int): Int {
    if width >= count - start { count } else { start + width }
}

pub fn sortedBy<T>(values: List<T>, key: (T) -> Int): List<T> {
    var source: List<SortEntry<T>> = []
    for item in values { source.add(SortEntry(key(item), item)) }
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
    map(source) { it.item }
}

pub fn sum(values: List<Int>): Int {
    var total = 0
    for value in values { total += value }
    total
}

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

pub extension ListOps<T> for List<T> {
    fn map<R>(transform: (T) -> R): List<R> { map(this, transform) }
    fn filter(predicate: (T) -> Bool): List<T> { filter(this, predicate) }
    fn fold<R>(initial: R, combine: (R, T) -> R): R { fold(this, initial, combine) }
    fn any(predicate: (T) -> Bool): Bool { any(this, predicate) }
    fn find(predicate: (T) -> Bool): Option<T> { find(this, predicate) }
    fn sortedBy(key: (T) -> Int): List<T> { sortedBy(this, key) }
}
