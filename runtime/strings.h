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
uint8_t lyraUtf8Valid(const char* bytes, size_t length);
const void* lyraStringConcat(const void* left, const void* right);
const void* lyraStringJoin(size_t count, const void* const* parts);
uint8_t lyraStringEqual(const void* left, const void* right);
uint64_t lyraStringHash(const void* value);
size_t lyraStringScalarCount(const void* text);
uint8_t lyraBytesUtf8(const void* bytes);
uint32_t lyraStringAtByte(const void* text, size_t offset);
uint8_t lyraBytesAtByte(const void* bytes, size_t offset);
uint32_t lyraStringIndex(const void* text, int64_t index, const char* file, uint64_t line, uint64_t column);
uint8_t lyraBytesIndex(const void* bytes, int64_t index, const char* file, uint64_t line, uint64_t column);
uint32_t lyraCharWidth(uint32_t codePoint);
const void* lyraCharText(uint32_t codePoint);
const void* lyraIntText(int64_t value);
const void* lyraUIntText(uint64_t value);
const void* lyraFloat32Text(float value);
const void* lyraFloat64Text(double value);
void lyraPrintString(const void* value);
int lyraReportStringError(const void* value);

#ifdef __cplusplus
}
#endif

#endif
