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
