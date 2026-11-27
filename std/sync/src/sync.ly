extern fn lyraMutexCreate(): CPtr;
extern fn lyraMutexLock(mutex: CPtr): Unit;
extern fn lyraMutexUnlock(mutex: CPtr): Unit;
extern fn lyraAtomicIntCreate(value: Int): CPtr;
extern fn lyraAtomicIntLoad(number: CPtr): Int;
extern fn lyraAtomicIntStore(number: CPtr, value: Int): Unit;
extern fn lyraAtomicIntFetchAdd(number: CPtr, amount: Int): Int;
extern fn lyraAtomicIntCompareExchange(number: CPtr, expected: Int, replacement: Int): Bool;

/// 可重入互斥锁，使用 `withLock` 管理持锁区间。
pub class Mutex {
    val handle: CPtr
    /// 创建一把独立、未持有的锁。
    pub init() { this.handle = lyraMutexCreate() }
}

/// 持锁执行同步闭包并返回其结果；正常返回或抛异常都会释放锁。
/// 闭包不能挂起，不应等待需要同一执行域推进的任务。
pub fn withLock<T>(mutex: Mutex, body: () -> T): T {
    val result = {
        lyraMutexLock(mutex.handle)
        defer lyraMutexUnlock(mutex.handle)
        body()
    }
    Task.checkCancellation()
    result
}

/// A shared value whose read/compute/replace operation is serialized.
pub class Locked<T> {
    val mutex: Mutex = Mutex()
    var value: T

    /// 创建有独立互斥锁的共享值。
    pub init(initial: T) { value = initial }
    /// 在锁内读取当前值；如果值是对象引用，不会深拷贝对象。
    pub fn snapshot(): T { withLock(mutex) { value } }
    /// 在同一次持锁期间读取旧值、计算并替换；闭包抛异常时不执行替换。
    pub fn update(change: (T) -> T): Unit { withLock(mutex) { value = change(value) } }
}

/// 顺序一致的有符号 64 位原子整数，可在多个执行域间共享。
pub class AtomicInt {
    val handle: CPtr
    /// 创建独立的原子整数，默认从 `0` 开始。
    pub init(value: Int = 0) { this.handle = lyraAtomicIntCreate(value) }
    /// 原子读取当前值。
    pub fn load(): Int { lyraAtomicIntLoad(handle) }
    /// 原子覆盖当前值。
    pub fn store(value: Int): Unit { lyraAtomicIntStore(handle, value) }
    /// 原子加上 `amount` 并返回加法前的值；溢出按二补码回绕。
    pub fn fetchAdd(amount: Int): Int { lyraAtomicIntFetchAdd(handle, amount) }
    /// 仅当当前值等于 `expected` 才替换，并返回是否替换成功。
    pub fn compareExchange(expected: Int, replacement: Int): Bool { lyraAtomicIntCompareExchange(handle, expected, replacement) }
}
