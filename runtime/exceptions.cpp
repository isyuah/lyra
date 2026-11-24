#include "exceptions.h"
#include "backtrace.h"
#include "managed.h"
#include "strings.h"

#include <cinttypes>
#include <cstdio>
#include <new>
#include <utility>

namespace {

struct Location { const char* file; std::uint64_t line, column; };
struct Frame { const char* role; Location location; const Frame* previous; };
struct Suppressed { const LyraException* error; const Suppressed* previous; std::size_t count; };

template<class Item, class... Args> Item* managed(Args&&... args) {
    return ::new (lyraGcAlloc(sizeof(Item))) Item{std::forward<Args>(args)...};
}

void location(FILE* stream, Location point) {
    std::fprintf(stream, "%s:%" PRIu64 ":%" PRIu64, point.file, point.line, point.column);
}

}

struct LyraException { const void* message; const void* type; const char* name; Location origin; const Frame* trace; const Suppressed* suppressed; const LyraException* identity; const lyraRuntime::Backtrace* native; };

const LyraException* lyraExceptionCreate(const void* message) {
    return lyraExceptionCreateTyped(message, nullptr, "Exception");
}

const LyraException* lyraExceptionCreateTyped(const void* message, const void* type, const char* name) {
    auto error = managed<LyraException>(message, type, name, Location{}, nullptr, nullptr, nullptr, nullptr);
    error->identity = error;
    return error;
}

const void* lyraExceptionMessage(const LyraException* error) { return error->message; }
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

std::size_t lyraExceptionSuppressedCount(const LyraException* error) { return error->suppressed ? error->suppressed->count : 0; }

const LyraException* lyraExceptionSuppress(const LyraException* primary, const LyraException* additional) {
    if (!primary) return additional;
    auto merged = managed<LyraException>(*primary);
    merged->suppressed = managed<Suppressed>(additional, primary->suppressed, lyraExceptionSuppressedCount(primary) + 1);
    return merged;
}

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
