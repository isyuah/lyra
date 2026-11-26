#include "sync.h"
#include "managed.h"

#include <atomic>
#include <cstdlib>
#include <gc/gc.h>
#include <new>
#include <uv.h>

struct LyraMutex {
    uv_mutex_t handle;
};

struct LyraAtomicInt {
    std::atomic<int64_t> value;
};

static_assert(std::atomic<int64_t>::is_always_lock_free);

LyraMutex *lyraMutexCreate() {
    auto *mutex = ::new (lyraGcAllocAtomic(sizeof(LyraMutex))) LyraMutex{};
    if (uv_mutex_init_recursive(&mutex->handle)) std::abort();
    GC_REGISTER_FINALIZER_NO_ORDER(mutex, [](void *object, void *) {
        uv_mutex_destroy(&static_cast<LyraMutex *>(object)->handle);
    }, nullptr, nullptr, nullptr);
    return mutex;
}

void lyraMutexLock(LyraMutex *mutex) {
    uv_mutex_lock(&mutex->handle);
    lyraCancellationShieldEnter();
    GC_reachable_here(mutex);
}

void lyraMutexUnlock(LyraMutex *mutex) {
    uv_mutex_unlock(&mutex->handle);
    lyraCancellationShieldLeave();
    GC_reachable_here(mutex);
}

LyraAtomicInt *lyraAtomicIntCreate(int64_t value) {
    return ::new (lyraGcAllocAtomic(sizeof(LyraAtomicInt))) LyraAtomicInt{value};
}

int64_t lyraAtomicIntLoad(const LyraAtomicInt *number) {
    return number->value.load(std::memory_order_seq_cst);
}

void lyraAtomicIntStore(LyraAtomicInt *number, int64_t value) {
    number->value.store(value, std::memory_order_seq_cst);
}

int64_t lyraAtomicIntFetchAdd(LyraAtomicInt *number, int64_t amount) {
    return number->value.fetch_add(amount, std::memory_order_seq_cst);
}

uint8_t lyraAtomicIntCompareExchange(LyraAtomicInt *number, int64_t expected, int64_t replacement) {
    return number->value.compare_exchange_strong(expected, replacement, std::memory_order_seq_cst);
}
