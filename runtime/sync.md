# 原生同步 ABI

- `lyraMutexCreate` 创建 GC 托管的可重入互斥锁。调用 `lyraMutexLock` 可能阻塞 OS 线程；同一个线程可以嵌套加锁，每次调用都要由该线程对应解锁。持锁期间保持句柄可达，不跨异步挂起点。原生锁的内部状态只有在句柄不可达时才销毁。
- `lyraAtomicIntCreate` 创建 GC 托管的 64 位整数。`load`、`store`、`fetchAdd` 和 `compareExchange` 均为顺序一致操作，不暴露内存序选项。`fetchAdd` 返回旧值，发生溢出时按二补码回绕。`compareExchange` 仅在当前值等于 `expected` 时写入 `replacement`，返回是否写入。
- [原生验收](../tests/runtime_sync.cpp)在两个并行域之间共享同一把锁和一个原子计数器，验证持锁更新、原子加法及跨线程 GC 后引用的可见性。
