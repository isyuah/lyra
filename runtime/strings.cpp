#include "managed.h"
#include "strings.h"
#include <lyra/string.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

std::size_t lyraStringLength(const void* string) {
    std::size_t result;
    std::memcpy(&result, string, sizeof(result));
    return result;
}

const char* lyraStringBytes(const void* string) {
    return static_cast<const char*>(string) + sizeof(std::size_t);
}

LyraStringView lyraStringView(const LyraString* value) {
    if (!value) return {nullptr, 0};
    return {lyraStringBytes(value), lyraStringLength(value)};
}

uint8_t lyraUtf8Valid(const char* bytes, std::size_t size) {
    for (std::size_t index = 0; index < size;) {
        auto lead = static_cast<unsigned char>(bytes[index++]);
        if (lead < 0x80) continue;
        if (lead < 0xc2 || lead > 0xf4) return false;
        auto count = lead < 0xe0 ? 1u : lead < 0xf0 ? 2u : 3u;
        if (size - index < count) return false;
        auto first = static_cast<unsigned char>(bytes[index]);
        if ((lead == 0xe0 && first < 0xa0) || (lead == 0xed && first >= 0xa0) ||
            (lead == 0xf0 && first < 0x90) || (lead == 0xf4 && first >= 0x90)) return false;
        for (unsigned item = 0; item < count; ++item) if ((static_cast<unsigned char>(bytes[index++]) & 0xc0) != 0x80) return false;
    }
    return true;
}

static char* allocate(std::size_t first, std::size_t second) {
    auto available = std::numeric_limits<std::size_t>::max() - sizeof(std::size_t);
    if (first > available || second > available - first) {
        std::fputs("Lyra runtime: string is too large\n", stderr);
        std::abort();
    }
    auto combined = first + second;
    auto* memory = static_cast<char*>(lyraGcAllocAtomic(sizeof(combined) + combined));
    std::memcpy(memory, &combined, sizeof(combined));
    return memory;
}

const void* lyraStringFromBytes(const char* bytes, std::size_t count) {
    auto* memory = allocate(count, 0);
    if (count) std::memcpy(memory + sizeof(count), bytes, count);
    return memory;
}

const void* lyraStringConcat(const void* left, const void* right) {
    auto first = lyraStringLength(left);
    auto second = lyraStringLength(right);
    auto* memory = allocate(first, second);
    std::memcpy(memory + sizeof(first), lyraStringBytes(left), first);
    std::memcpy(memory + sizeof(first) + first, lyraStringBytes(right), second);
    return memory;
}

std::uint8_t lyraStringEqual(const void* left, const void* right) {
    auto count = lyraStringLength(left);
    return count == lyraStringLength(right) && std::memcmp(lyraStringBytes(left), lyraStringBytes(right), count) == 0;
}

void lyraPrintString(const void* value) {
    std::fwrite(lyraStringBytes(value), 1, lyraStringLength(value), stdout);
    std::putchar('\n');
}

int lyraReportStringError(const void* value) {
    std::fwrite(lyraStringBytes(value), 1, lyraStringLength(value), stderr);
    std::fputc('\n', stderr);
    return EXIT_FAILURE;
}
