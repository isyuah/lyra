#include "managed.h"

#define GC_THREADS
#include <gc/gc.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <uv.h>

struct LyraGcRoot {
    void *value;
};

struct LyraCleanup {
    LyraCleanupCall call;
    void *environment;
    LyraCleanup *next;
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
        if (auto *failure = top->call(top->environment)) primary = lyraExceptionSuppress(primary, failure);
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
