#include "exceptions.h"
#include "backtrace.h"
#include "lists.h"
#include "managed.h"
#include "strings.h"

#include <cinttypes>
#include <cstdio>
#include <new>
#include <utility>

namespace {

struct Location { const char* file; std::uint64_t line, column; };
struct Frame { const char* role; Location location; const Frame* previous; };
struct Failures { const LyraException* error; const Failures* previous; std::size_t count; };

template<class Item, class... Args> Item* managed(Args&&... args) {
    return ::new (lyraGcAlloc(sizeof(Item))) Item{std::forward<Args>(args)...};
}

void location(FILE* stream, Location point) {
    std::fprintf(stream, "%s:%" PRIu64 ":%" PRIu64, point.file, point.line, point.column);
}

}

struct LyraException {
    const void* message;
    const void* type;
    const char* name;
    Location origin{};
    const Frame* trace{};
    const Failures *suppressed{}, *tasks{}, *cleanups{};
    const LyraException* identity{};
    const lyraRuntime::Backtrace* native{};
    const void* failure{};
};

namespace {

LyraException* createException(const void* message, const void* type, const char* name) {
    auto* error = managed<LyraException>(message, type, name);
    error->identity = error;
    return error;
}

std::size_t failureCount(const Failures* list) { return list ? list->count : 0; }

const Failures* append(const Failures* list, const LyraException* error) {
    return managed<Failures>(error, list, failureCount(list) + 1);
}

const LyraException* record(const LyraException* primary, const LyraException* failure, const Failures* LyraException::* category) {
    auto* result = managed<LyraException>(*(primary ? primary : failure));
    if (primary) result->suppressed = append(primary->suppressed, failure);
    else result->tasks = result->cleanups = nullptr;
    result->*category = append(result->*category, failure);
    return result;
}

LyraList* snapshot(const Failures* entries) {
    auto* result = lyraListCreate();
    if (!entries) return result;
    auto count = entries->count;
    auto** ordered = static_cast<const LyraException**>(lyraGcAlloc(count * sizeof(const LyraException*)));
    for (auto entry = entries; entry; entry = entry->previous) ordered[entry->count - 1] = entry->error;
    for (std::size_t index = 0; index < count; ++index)
        lyraListAdd(result, ::new (lyraGcAlloc(sizeof(*ordered))) const LyraException*(ordered[index]));
    return result;
}

}

const LyraException* lyraExceptionCreate(const void* message) {
    return lyraExceptionCreateTyped(message, nullptr, "Exception");
}

const LyraException* lyraExceptionCreateTyped(const void* message, const void* type, const char* name) {
    return createException(message, type, name);
}

const LyraException* lyraExceptionCreateUnwrap(const void* message, const void* type, const void* failure) {
    auto* error = createException(message, type, "UnwrapError");
    error->failure = failure;
    return error;
}

const LyraException* lyraIndexError(const void* type, const char* collection, std::int64_t index, std::size_t size) {
    char message[128];
    auto length = std::snprintf(message, sizeof(message), "%s index %" PRId64 " out of bounds (size %zu)", collection, index, size);
    return lyraExceptionCreateTyped(lyraStringFromBytes(message, length), type, "BoundsError");
}

const void* lyraExceptionMessage(const LyraException* error) { return error->message; }
const void* lyraExceptionFailure(const LyraException* error) { return error->failure; }
const void* lyraExceptionType(const LyraException* error) { return error->type; }
int lyraExceptionSame(const LyraException* left, const LyraException* right) { return left->identity == right->identity; }

const LyraException* lyraExceptionRaise(const LyraException* error, const char* file, std::uint64_t line, std::uint64_t column) {
    if (error->origin.file) return error;
    auto located = managed<LyraException>(*error);
    located->origin = {file, line, column};
    located->native = lyraRuntime::captureBacktrace();
    return located;
}

const LyraException* lyraExceptionFrame(const LyraException* error, const char* role, const char* file, std::uint64_t line, std::uint64_t column) {
    auto traced = managed<LyraException>(*error);
    traced->trace = managed<Frame>(role, Location{file, line, column}, error->trace);
    return traced;
}

std::size_t lyraExceptionSuppressedCount(const LyraException* error) { return failureCount(error->suppressed); }

const LyraException* lyraExceptionAddTaskFailure(const LyraException* primary, const LyraException* failure) {
    return record(primary, failure, &LyraException::tasks);
}

const LyraException* lyraExceptionAddCleanupFailure(const LyraException* primary, const LyraException* failure) {
    return record(primary, failure, &LyraException::cleanups);
}

LyraList* lyraExceptionTaskFailures(const LyraException* error) { return snapshot(error->tasks); }
LyraList* lyraExceptionCleanupFailures(const LyraException* error) { return snapshot(error->cleanups); }

void lyraExceptionWrite(const LyraException* error, FILE* stream) {
    struct Pending { const LyraException* error; const Pending* next; };
    struct Trace { const Frame* frame; const Trace* next; };
    Pending first{error, nullptr};
    for (const Pending* pending = &first; pending;) {
        auto item = pending->error;
        if (pending != &first) std::fputs("suppressed ", stream);
        std::fprintf(stream, "%s: ", item->name);
        pending = pending->next;
        std::fwrite(lyraStringBytes(item->message), 1, lyraStringLength(item->message), stream);
        std::fputc('\n', stream);
        if (item->origin.file) { std::fputs("  throw at ", stream); location(stream, item->origin); std::fputc('\n', stream); }
        const Trace* trace = nullptr;
        for (auto frame = item->trace; frame; frame = frame->previous) trace = managed<Trace>(frame, trace);
        for (; trace; trace = trace->next) {
            std::fprintf(stream, "  %s at ", trace->frame->role); location(stream, trace->frame->location); std::fputc('\n', stream);
        }
        if (item->native) lyraRuntime::writeBacktrace(*item->native, stream);
        for (auto extra = item->suppressed; extra; extra = extra->previous) pending = managed<Pending>(extra->error, pending);
    }
}

void lyraExceptionReport(const LyraException* error) { lyraExceptionWrite(error, stderr); }
