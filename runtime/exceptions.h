#ifndef LYRA_RUNTIME_EXCEPTIONS_H
#define LYRA_RUNTIME_EXCEPTIONS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LyraException LyraException;
const LyraException* lyraExceptionCreate(const void* message);
const void* lyraExceptionMessage(const LyraException* error);
const LyraException* lyraExceptionRaise(const LyraException* error, const char* file, uint64_t line, uint64_t column);
const LyraException* lyraExceptionFrame(const LyraException* error, const char* role, const char* file, uint64_t line, uint64_t column);
const LyraException* lyraExceptionSuppress(const LyraException* primary, const LyraException* additional);
size_t lyraExceptionSuppressedCount(const LyraException* error);
void lyraExceptionWrite(const LyraException* error, FILE* stream);
void lyraExceptionReport(const LyraException* error);

#ifdef __cplusplus
}
#endif

#endif
