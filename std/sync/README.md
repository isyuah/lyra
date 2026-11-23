# std.sync

`Mutex()` 创建可重入互斥锁，`withLock(mutex) { ... }` 执行一个同步临界区。正常完成或抛异常都会释放锁，返回值就是块的结果。锁内不能 `await` 或 `spawn`；不要持锁做长时间同步阻塞，也不应等待自己的逻辑域推进。

`AtomicInt(value)` 创建顺序一致的 64 位原子整数，默认值为零。`load()` 读取、`store(value)` 写入、`fetchAdd(amount)` 加法返回旧值（溢出按二补码回绕）、`compareExchange(expected, replacement)` 返回是否替换成功。

Class 字段、捕获绑定及 List 的单次访问不会因跨域读写而读出撕裂的引用。复合操作不是原子的；多个域共同操作 Map/Set 时，用同一把锁保护查找、更新与遍历。
