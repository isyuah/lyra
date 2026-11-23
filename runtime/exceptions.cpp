#include "exceptions.h"
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

struct LyraException { const void* message; Location origin; const Frame* trace; const Suppressed* suppressed; const LyraException* identity; };

const LyraException* lyraExceptionCreate(const void* message) {
    auto error = managed<LyraException>(message, Location{}, nullptr, nullptr, nullptr);
    error->identity = error;
    return error;
}

const void* lyraExceptionMessage(const LyraException* error) { return error->message; }
int lyraExceptionSame(const LyraException* left, const LyraException* right) { return left->identity == right->identity; }

const LyraException* lyraExceptionRaise(const LyraException* error, const char* file, std::uint64_t line, std::uint64_t column) {
    if (error->origin.file) return error;
    return managed<LyraException>(error->message, Location{file, line, column}, error->trace, error->suppressed, error->identity);
}

const LyraException* lyraExceptionFrame(const LyraException* error, const char* role, const char* file, std::uint64_t line, std::uint64_t column) {
    auto trace = managed<Frame>(role, Location{file, line, column}, error->trace);
    return managed<LyraException>(error->message, error->origin, trace, error->suppressed, error->identity);
}

std::size_t lyraExceptionSuppressedCount(const LyraException* error) { return error->suppressed ? error->suppressed->count : 0; }

const LyraException* lyraExceptionSuppress(const LyraException* primary, const LyraException* additional) {
    if (!primary) return additional;
    auto chain = managed<Suppressed>(additional, primary->suppressed, lyraExceptionSuppressedCount(primary) + 1);
    return managed<LyraException>(primary->message, primary->origin, primary->trace, chain, primary->identity);
}

void lyraExceptionWrite(const LyraException* error, FILE* stream) {
    struct Pending { const LyraException* error; const Pending* next; };
    struct Trace { const Frame* frame; const Trace* next; };
    Pending first{error, nullptr};
    for (const Pending* pending = &first; pending;) {
        auto item = pending->error;
        std::fputs(pending == &first ? "Exception: " : "suppressed Exception: ", stream);
        pending = pending->next;
        std::fwrite(lyraStringBytes(item->message), 1, lyraStringLength(item->message), stream);
        std::fputc('\n', stream);
        if (item->origin.file) { std::fputs("  throw at ", stream); location(stream, item->origin); std::fputc('\n', stream); }
        const Trace* trace = nullptr;
        for (auto frame = item->trace; frame; frame = frame->previous) trace = managed<Trace>(frame, trace);
        for (; trace; trace = trace->next) {
            std::fprintf(stream, "  %s at ", trace->frame->role); location(stream, trace->frame->location); std::fputc('\n', stream);
        }
        for (auto extra = item->suppressed; extra; extra = extra->previous) pending = managed<Pending>(extra->error, pending);
    }
}

void lyraExceptionReport(const LyraException* error) { lyraExceptionWrite(error, stderr); }
