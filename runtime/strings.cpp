#include "managed.h"
#include "strings.h"

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
