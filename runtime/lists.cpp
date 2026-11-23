#include "lists.h"
#include "locks.h"
#include "managed.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

struct LyraList { const void** items; std::size_t count, capacity; };

namespace {

constexpr std::size_t stripes = 32;
uv_mutex_t listLocks[stripes];
uv_once_t initialized = UV_ONCE_INIT;

std::size_t stripe(const LyraList* list) { return (reinterpret_cast<std::uintptr_t>(list) >> 4) % stripes; }

void reserve(LyraList* list, std::size_t extra) {
    constexpr auto limit = std::numeric_limits<std::size_t>::max() / sizeof(*list->items);
    if (extra > limit - list->count) {
        std::fputs("Lyra runtime: list is too large\n", stderr);
        std::abort();
    }
    auto needed = list->count + extra;
    if (needed <= list->capacity) return;
    auto growth = list->capacity > limit / 2 ? limit : std::max<std::size_t>(4, list->capacity * 2);
    auto capacity = std::max(needed, growth);
    auto items = static_cast<const void**>(lyraGcAlloc(capacity * sizeof(*list->items)));
    if (list->count) std::memcpy(items, list->items, list->count * sizeof(*items));
    list->items = items;
    list->capacity = capacity;
}

void lockedLists(LyraList* list, const LyraList* source, const auto& work) {
    auto first = stripe(list), second = stripe(source);
    lyraRuntime::Lock lower(listLocks[std::min(first, second)]);
    if (first == second) work();
    else { lyraRuntime::Lock upper(listLocks[std::max(first, second)]); work(); }
}

}

LyraList* lyraListCreate() {
    uv_once(&initialized, [] { for (auto& mutex : listLocks) lyraRuntime::initMutex(mutex); });
    return ::new (lyraGcAlloc(sizeof(LyraList))) LyraList{};
}

std::int64_t lyraListSize(const LyraList* list) {
    lyraRuntime::Lock guard(listLocks[stripe(list)]);
    return static_cast<std::int64_t>(list->count);
}

const void* lyraListGet(const LyraList* list, std::int64_t index, const char* file, std::uint64_t line, std::uint64_t column) {
    lyraRuntime::Lock guard(listLocks[stripe(list)]);
    if (index < 0 || static_cast<std::uint64_t>(index) >= list->count) {
        std::fprintf(stderr, "%s:%" PRIu64 ":%" PRIu64 ": list index %" PRId64 " out of bounds (size %zu)\n", file, line, column, index, list->count);
        std::exit(EXIT_FAILURE);
    }
    return list->items[index];
}

void lyraListSet(LyraList* list, std::int64_t index, const void* boxed) {
    lyraRuntime::Lock guard(listLocks[stripe(list)]);
    list->items[index] = boxed;
}

void lyraListAdd(LyraList* list, const void* boxed) {
    lyraRuntime::Lock guard(listLocks[stripe(list)]);
    reserve(list, 1);
    list->items[list->count++] = boxed;
}

void lyraListExtend(LyraList* list, const LyraList* source) {
    lockedLists(list, source, [&] {
        auto count = source->count;
        if (!count) return;
        reserve(list, count);
        std::memcpy(list->items + list->count, source->items, count * sizeof(*list->items));
        list->count += count;
    });
}

void lyraListExtendAny(LyraList* list, const LyraList* source, const void* type) {
    struct AnyValue { const void* type; const void* data; };
    lockedLists(list, source, [&] {
        auto count = source->count;
        reserve(list, count);
        for (std::size_t index = 0; index < count; ++index) {
            auto value = ::new (lyraGcAlloc(sizeof(AnyValue))) AnyValue{type, source->items[index]};
            list->items[list->count++] = value;
        }
    });
}
