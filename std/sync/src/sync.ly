extern fn lyraMutexCreate(): CPtr;
extern fn lyraMutexLock(mutex: CPtr): Unit;
extern fn lyraMutexUnlock(mutex: CPtr): Unit;
extern fn lyraAtomicIntCreate(value: Int): CPtr;
extern fn lyraAtomicIntLoad(number: CPtr): Int;
extern fn lyraAtomicIntStore(number: CPtr, value: Int): Unit;
extern fn lyraAtomicIntFetchAdd(number: CPtr, amount: Int): Int;
extern fn lyraAtomicIntCompareExchange(number: CPtr, expected: Int, replacement: Int): Bool;

pub class Mutex {
    val handle: CPtr
    pub init() { this.handle = lyraMutexCreate() }
}

pub fn withLock<T>(mutex: Mutex, body: () -> T): T {
    lyraMutexLock(mutex.handle)
    defer lyraMutexUnlock(mutex.handle)
    body()
}

pub class AtomicInt {
    val handle: CPtr
    pub init(value: Int = 0) { this.handle = lyraAtomicIntCreate(value) }
    pub fn load(): Int { lyraAtomicIntLoad(handle) }
    pub fn store(value: Int): Unit { lyraAtomicIntStore(handle, value) }
    pub fn fetchAdd(amount: Int): Int { lyraAtomicIntFetchAdd(handle, amount) }
    pub fn compareExchange(expected: Int, replacement: Int): Bool { lyraAtomicIntCompareExchange(handle, expected, replacement) }
}
