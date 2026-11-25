#include "managed.h"

#define GC_THREADS
#include <gc/gc.h>
#include <atomic>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string_view>
#include <uv.h>

#include "strings.h"

struct LyraGcRoot {
    void *value;
};

struct LyraCleanup {
    LyraCleanupCall call;
    void *environment;
    LyraCleanup *next;
};

struct LyraCallback {
    void* closure;
    void* function;
    void* fallback;
    std::atomic<LyraGcRoot*> root{};
    std::atomic<const LyraException*> error{};
};

namespace {

uv_once_t initialization = UV_ONCE_INIT;

void initialize() {
    GC_INIT();
    GC_allow_register_threads();
}

void *requireMemory(void *memory) {
    if (!memory) {
        fputs("Lyra runtime: managed allocation failed\n", stderr);
        abort();
    }
    return memory;
}

}

void *lyraGcAlloc(size_t bytes) {
    uv_once(&initialization, initialize);
    return requireMemory(GC_MALLOC(bytes ? bytes : 1));
}

void *lyraGcAllocAtomic(size_t bytes) {
    uv_once(&initialization, initialize);
    return requireMemory(GC_MALLOC_ATOMIC(bytes ? bytes : 1));
}

void lyraGcCollect() {
    uv_once(&initialization, initialize);
    GC_gcollect();
}

LyraGcRoot *lyraGcRootCreate(void *value) {
    uv_once(&initialization, initialize);
    return ::new (requireMemory(GC_MALLOC_UNCOLLECTABLE(sizeof(LyraGcRoot)))) LyraGcRoot{value};
}

void *lyraGcRootGet(const LyraGcRoot *root) {
    return root->value;
}

void lyraGcRootDrop(LyraGcRoot *root) {
    GC_FREE(root);
}

void lyraCleanupPush(LyraCleanup **stack, LyraCleanupCall call, void *environment) {
    *stack = ::new (lyraGcAlloc(sizeof(LyraCleanup))) LyraCleanup{call, environment, *stack};
}

const LyraException *lyraCleanupUntil(LyraCleanup **stack, const LyraCleanup *mark, const LyraException *primary) {
    while (*stack != mark) {
        auto *top = *stack;
        *stack = top->next;
        if (auto *failure = top->call(top->environment)) primary = lyraExceptionAddCleanupFailure(primary, failure);
    }
    return primary;
}

int lyraGcEnterWorker() {
    if (GC_thread_is_registered()) return 0;
    GC_stack_base base;
    if (GC_get_stack_base(&base) != GC_SUCCESS) abort();
    auto status = GC_register_my_thread(&base);
    if (status == GC_SUCCESS) return 1;
    if (status == GC_DUPLICATE) return 0;
    abort();
}

void lyraGcLeaveWorker(int registration) {
    if (registration && GC_unregister_my_thread() != GC_SUCCESS) abort();
}

extern "C" void* lyraPtrAllocate(int64_t bytes, const LyraException** error) {
    if (bytes >= 0) {
        if (auto* memory = malloc(bytes ? static_cast<size_t>(bytes) : 1)) return memory;
    }
    const std::string_view message = bytes < 0 ? "原生分配长度不能为负数" : "原生内存分配失败";
    *error = lyraExceptionCreate(lyraStringFromBytes(message.data(), message.size()));
    return nullptr;
}

extern "C" LyraCallback* lyraCallbackRegister(void* closure, void* function, void* fallback) {
    auto* callback = ::new (lyraGcAlloc(sizeof(LyraCallback))) LyraCallback{closure, function, fallback};
    callback->root.store(lyraGcRootCreate(callback));
    return callback;
}

extern "C" void* lyraCallbackContext(LyraCallback* callback) { return callback->root.load() ? callback : nullptr; }
extern "C" void* lyraCallbackFunction(const LyraCallback* callback) { return callback->root.load() ? callback->function : nullptr; }
extern "C" void* lyraCallbackClosure(const LyraCallback* callback) { return callback->closure; }
extern "C" void* lyraCallbackFallback(const LyraCallback* callback) { return callback->fallback; }

extern "C" void lyraCallbackClose(LyraCallback* callback) {
    if (auto* root = callback->root.exchange(nullptr)) {
        callback->closure = callback->fallback = nullptr;
        lyraGcRootDrop(root);
    }
}

extern "C" void lyraCallbackRecordError(LyraCallback* callback, const LyraException* error) {
    const LyraException* pending = nullptr;
    callback->error.compare_exchange_strong(pending, error);
}

extern "C" const LyraException* lyraCallbackTakeError(LyraCallback* callback) { return callback->error.exchange(nullptr); }
