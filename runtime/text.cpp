#include "strings.h"

extern "C" const void* lyraTextCopyRange(const void* text, int64_t start, int64_t end) {
    if (start == 0 && static_cast<size_t>(end) == lyraStringLength(text)) return text;
    return lyraStringFromBytes(lyraStringBytes(text) + start, static_cast<size_t>(end - start));
}
