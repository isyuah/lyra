#include "lists.h"
#include "managed.h"
#include "strings.h"

#include <cstdio>
#include <cstdlib>
#include <new>

namespace {

struct Error {
    const void* type;
    const void* value;
    const void* message;
    const Error* cause;
    const LyraList* suppressed;
};

LyraList* copySuppressed(const Error* error) {
    auto* copy = lyraListCreate();
    if (error->suppressed) lyraListExtend(copy, error->suppressed);
    return copy;
}

}

extern "C" const Error* lyraErrorCreate(const void* type, const void* value, const void* message, const Error* cause) {
    return ::new (lyraGcAlloc(sizeof(Error))) Error{type, value, message, cause, nullptr};
}

extern "C" const void* lyraErrorType(const Error* error) { return error->type; }
extern "C" const void* lyraErrorValue(const Error* error) { return error->value; }
extern "C" const void* lyraErrorMessage(const Error* error) { return error->message; }
extern "C" const Error* lyraErrorCause(const Error* error) { return error->cause; }
extern "C" LyraList* lyraErrorSuppressed(const Error* error) { return copySuppressed(error); }

extern "C" const Error* lyraErrorSuppress(const Error* error, const Error* additional) {
    auto* list = copySuppressed(error);
    lyraListAdd(list, ::new (lyraGcAlloc(sizeof(additional))) const Error*(additional));
    auto* result = ::new (lyraGcAlloc(sizeof(Error))) Error(*error);
    result->suppressed = list;
    return result;
}

extern "C" int lyraErrorReport(const Error* error) {
    struct Line { const Error* error; const char* label; const Line* next; };
    auto push = [](const Error* value, const char* label, const Line* next) {
        return ::new (lyraGcAlloc(sizeof(Line))) Line{value, label, next};
    };
    const Line first{error, "Error: ", nullptr};
    for (const Line* pending = &first; pending;) {
        auto [record, label, next] = *pending;
        pending = next;
        std::fputs(label, stderr);
        lyraReportStringError(record->message);
        if (auto* suppressed = record->suppressed) {
            for (auto count = lyraListSize(suppressed); count > 0; --count) {
                auto* boxed = lyraFrozenListItem(suppressed, count - 1);
                pending = push(*static_cast<const Error* const*>(boxed), "suppressed: ", pending);
            }
        }
        if (record->cause) pending = push(record->cause, "caused by: ", pending);
    }
    return EXIT_FAILURE;
}
