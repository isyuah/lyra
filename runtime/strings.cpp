#include "managed.h"
#include "strings.h"
#include <lyra/string.h>
#include <lyra/utf8.hpp>

#include <cinttypes>
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

LyraBytesView lyraBytesView(const LyraBytes* value) {
    if (!value) return {nullptr, 0};
    return {reinterpret_cast<const unsigned char*>(lyraStringBytes(value)), lyraStringLength(value)};
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

static std::size_t combinedSize(std::size_t first, std::size_t second) {
    auto available = std::numeric_limits<std::size_t>::max() - sizeof(std::size_t);
    if (first > available || second > available - first) {
        std::fputs("Lyra runtime: string is too large\n", stderr);
        std::abort();
    }
    return first + second;
}

static char* allocate(std::size_t combined) {
    auto* memory = static_cast<char*>(lyraGcAllocAtomic(sizeof(combined) + combined));
    std::memcpy(memory, &combined, sizeof(combined));
    return memory;
}

const void* lyraStringFromBytes(const char* bytes, std::size_t count) {
    auto* memory = allocate(combinedSize(count, 0));
    if (count) std::memcpy(memory + sizeof(count), bytes, count);
    return memory;
}

const void* lyraStringConcat(const void* left, const void* right) {
    auto first = lyraStringLength(left);
    auto second = lyraStringLength(right);
    auto* memory = allocate(combinedSize(first, second));
    std::memcpy(memory + sizeof(first), lyraStringBytes(left), first);
    std::memcpy(memory + sizeof(first) + first, lyraStringBytes(right), second);
    return memory;
}

const void* lyraStringJoin(std::size_t count, const void* const* parts) {
    std::size_t length = 0;
    for (std::size_t index = 0; index < count; ++index) length = combinedSize(length, lyraStringLength(parts[index]));
    auto* memory = allocate(length);
    auto* out = memory + sizeof(length);
    for (std::size_t index = 0; index < count; ++index) {
        auto size = lyraStringLength(parts[index]);
        std::memcpy(out, lyraStringBytes(parts[index]), size);
        out += size;
    }
    return memory;
}

std::uint8_t lyraStringEqual(const void* left, const void* right) {
    auto count = lyraStringLength(left);
    return count == lyraStringLength(right) && std::memcmp(lyraStringBytes(left), lyraStringBytes(right), count) == 0;
}

std::size_t lyraStringScalarCount(const void* text) {
    auto bytes = lyraStringBytes(text);
    auto length = lyraStringLength(text);
    std::size_t count = 0;
    for (std::size_t index = 0; index < length; index += lyra::utf8::sequenceWidth(static_cast<unsigned char>(bytes[index]))) ++count;
    return count;
}

std::uint8_t lyraBytesUtf8(const void* bytes) { return lyraUtf8Valid(lyraStringBytes(bytes), lyraStringLength(bytes)); }

std::uint32_t lyraStringAtByte(const void* text, std::size_t offset) {
    auto bytes = lyraStringBytes(text) + offset;
    return lyra::utf8::decode(bytes, lyra::utf8::sequenceWidth(static_cast<unsigned char>(*bytes)));
}

std::uint8_t lyraBytesAtByte(const void* bytes, std::size_t offset) {
    return static_cast<std::uint8_t>(lyraStringBytes(bytes)[offset]);
}

[[noreturn]] static void indexError(const char* type, std::int64_t index, std::size_t size, const char* file, std::uint64_t line, std::uint64_t column) {
    std::fprintf(stderr, "%s:%" PRIu64 ":%" PRIu64 ": %s index %" PRId64 " out of bounds (size %zu)\n", file, line, column, type, index, size);
    std::exit(EXIT_FAILURE);
}

std::uint32_t lyraStringIndex(const void* text, std::int64_t index, const char* file, std::uint64_t line, std::uint64_t column) {
    auto bytes = lyraStringBytes(text);
    auto length = lyraStringLength(text);
    if (index < 0) indexError("String", index, lyraStringScalarCount(text), file, line, column);
    std::int64_t current = 0;
    for (std::size_t offset = 0; offset < length; ++current) {
        if (current == index) return lyraStringAtByte(text, offset);
        offset += lyra::utf8::sequenceWidth(static_cast<unsigned char>(bytes[offset]));
    }
    indexError("String", index, static_cast<std::size_t>(current), file, line, column);
}

std::uint8_t lyraBytesIndex(const void* bytes, std::int64_t index, const char* file, std::uint64_t line, std::uint64_t column) {
    auto size = lyraStringLength(bytes);
    if (index < 0 || static_cast<std::uint64_t>(index) >= size) indexError("Bytes", index, size, file, line, column);
    return lyraBytesAtByte(bytes, static_cast<std::size_t>(index));
}

std::uint32_t lyraCharWidth(std::uint32_t codePoint) { return lyra::utf8::width(codePoint); }

const void* lyraCharText(std::uint32_t codePoint) {
    char bytes[4];
    return lyraStringFromBytes(bytes, lyra::utf8::encode(codePoint, bytes));
}

template<class Numeric>
static const void* format(const char* specifier, Numeric value) {
    char bytes[64];
    auto size = std::snprintf(bytes, sizeof(bytes), specifier, value);
    if (size < 0 || static_cast<std::size_t>(size) >= sizeof(bytes)) std::abort();
    return lyraStringFromBytes(bytes, static_cast<std::size_t>(size));
}

const void* lyraIntText(std::int64_t value) { return format("%" PRId64, value); }
const void* lyraUIntText(std::uint64_t value) { return format("%" PRIu64, value); }
const void* lyraFloat32Text(float value) { return format("%.9g", static_cast<double>(value)); }
const void* lyraFloat64Text(double value) { return format("%.17g", value); }

void lyraPrintString(const void* value) {
    std::fwrite(lyraStringBytes(value), 1, lyraStringLength(value), stdout);
    std::putchar('\n');
    std::fflush(stdout);
}

int lyraReportStringError(const void* value) {
    std::fwrite(lyraStringBytes(value), 1, lyraStringLength(value), stderr);
    std::fputc('\n', stderr);
    return EXIT_FAILURE;
}
