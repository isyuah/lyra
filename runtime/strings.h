#ifndef LYRA_RUNTIME_STRINGS_H
#define LYRA_RUNTIME_STRINGS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t lyraStringLength(const void* string);
const char* lyraStringBytes(const void* string);
const void* lyraStringFromBytes(const char* bytes, size_t length);
const void* lyraStringConcat(const void* left, const void* right);
uint8_t lyraStringEqual(const void* left, const void* right);
void lyraPrintString(const void* value);

#ifdef __cplusplus
}
#endif

#endif
