#ifndef LYRA_RUNTIME_EXCEPTIONS_H
#define LYRA_RUNTIME_EXCEPTIONS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LyraException LyraException;
typedef struct LyraList LyraList;
const LyraException* lyraExceptionCreate(const void* message);
const LyraException* lyraExceptionCreateTyped(const void* message, const void* type, const char* name);
const LyraException* lyraExceptionCreateUnwrap(const void* message, const void* type, const void* failure);
const LyraException* lyraIndexError(const void* type, const char* collection, int64_t index, size_t size);
const void* lyraExceptionMessage(const LyraException* error);
const void* lyraExceptionFailure(const LyraException* error);
const void* lyraExceptionType(const LyraException* error);
int lyraExceptionSame(const LyraException* left, const LyraException* right);
const LyraException* lyraExceptionRaise(const LyraException* error, const char* file, uint64_t line, uint64_t column);
const LyraException* lyraExceptionFrame(const LyraException* error, const char* role, const char* file, uint64_t line, uint64_t column);
const LyraException* lyraExceptionAddTaskFailure(const LyraException* primary, const LyraException* failure);
const LyraException* lyraExceptionAddCleanupFailure(const LyraException* primary, const LyraException* failure);
LyraList* lyraExceptionTaskFailures(const LyraException* error);
LyraList* lyraExceptionCleanupFailures(const LyraException* error);
size_t lyraExceptionSuppressedCount(const LyraException* error);
void lyraExceptionWrite(const LyraException* error, FILE* stream);
void lyraExceptionReport(const LyraException* error);

#ifdef __cplusplus
}
#endif

#endif
