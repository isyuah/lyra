#include "context.h"
#include "managed.h"
#include "strings.h"

#include <algorithm>
#include <new>
#include <uv.h>

struct ContextLocal {
    const void *key;
    void *value;
    const ContextLocal *next;
};

struct LyraContext {
    const void *trace;
    int64_t deadline;
    const ContextLocal *locals;
};

namespace {
LyraContext *derived(const LyraContext *parent) {
    return ::new (lyraGcAlloc(sizeof(LyraContext))) LyraContext(parent ? *parent : LyraContext{nullptr, -1, nullptr});
}
}

const LyraContext *lyraContextWithTrace(const LyraContext *context, const void *trace) {
    auto *scope = derived(context);
    scope->trace = trace;
    return scope;
}

const LyraContext *lyraContextWithTimeout(const LyraContext *context, int64_t milliseconds) {
    auto *scope = derived(context);
    auto now = lyraContextClock();
    auto delay = std::clamp<int64_t>(milliseconds, 0, INT64_MAX - now);
    auto requested = now + delay;
    if (scope->deadline < 0 || requested < scope->deadline) scope->deadline = requested;
    return scope;
}

const LyraContext *lyraContextWithLocal(const LyraContext *context, const void *key, void *value) {
    auto *scope = derived(context);
    scope->locals = ::new (lyraGcAlloc(sizeof(ContextLocal))) ContextLocal{key, value, scope->locals};
    return scope;
}

void *lyraContextLocal(const LyraContext *context, const void *key) {
    for (auto *entry = context ? context->locals : nullptr; entry; entry = entry->next)
        if (entry->key == key) return entry->value;
    return nullptr;
}

const void *lyraContextTrace(const LyraContext *context) {
    return context && context->trace ? context->trace : lyraStringFromBytes(nullptr, 0);
}

int64_t lyraContextDeadline(const LyraContext *context) { return context ? context->deadline : -1; }
int64_t lyraContextClock() { return static_cast<int64_t>(uv_hrtime() / UINT64_C(1000000)); }
int64_t lyraContextRemaining(const LyraContext *context) {
    auto deadline = lyraContextDeadline(context);
    return deadline < 0 ? -1 : std::max<int64_t>(0, deadline - lyraContextClock());
}
