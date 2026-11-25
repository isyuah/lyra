import std.collections.{parallelFor}
import std.io.{readLine}
import std.sync.{AtomicInt}
import std.task.{parallelism}

extern fn uv_hrtime(): UInt;

const primeLimit: Int = 2000000
const chunkSize: Int = 16384

struct PrimeStats(val count: Int, val total: Int)

fn isPrime(number: Int): Bool {
    if number % 2 == 0 { return number == 2 }
    var divisor = 3
    while divisor * divisor <= number {
        if number % divisor == 0 { return false }
        divisor += 2
    }
    true
}

fn countPrimes(start: Int): PrimeStats {
    val end = if start + chunkSize < primeLimit { start + chunkSize } else { primeLimit }
    var count = 0
    var total = 0
    for number in start..<end {
        if isPrime(number) {
            count += 1
            total += number
        }
    }
    PrimeStats(count, total)
}

async fn addChunk(start: Int, count: AtomicInt, total: AtomicInt): Unit {
    val stats = countPrimes(start)
    count.fetchAdd(stats.count)
    total.fetchAdd(stats.total);
}

async fn main(): Unit {
    val chunks: List<Int> = []
    var start = 2
    while start < primeLimit {
        chunks.add(start)
        start += chunkSize
    }

    print(parallelism())
    while true {
        match readLine() {
            Ok(None) => break,
            Ok(Some(_)) => {},
            Err(error) => throw Exception(error.message()),
        }
        val count = AtomicInt()
        val total = AtomicInt()
        val started = uv_hrtime()
        await parallelFor(chunks) { addChunk(it, count, total) }
        val elapsed = uv_hrtime() - started
        print("${elapsed} ${count.load()} ${total.load()}")
    }
}
