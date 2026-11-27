#include "strings.h"
#include "managed.h"
#include <lyra/utf8.hpp>

#include <charconv>
#include <cmath>
#include <string>

extern "C" const void* lyraTextCopyRange(const void* text, int64_t start, int64_t end) {
    if (start == 0 && static_cast<size_t>(end) == lyraStringLength(text)) return text;
    return lyraStringFromBytes(lyraStringBytes(text) + start, static_cast<size_t>(end - start));
}

extern "C" const void** lyraTextParts(int64_t count) {
    return static_cast<const void**>(lyraGcAlloc(static_cast<std::size_t>(count) * sizeof(void*)));
}

extern "C" void lyraTextPart(const void** parts, int64_t index, const void* text) { parts[index] = text; }

extern "C" const void* lyraTextInsertAtScalars(const void* text, const void* inserted) {
    std::string_view input(lyraStringBytes(text), lyraStringLength(text));
    std::string_view separator(lyraStringBytes(inserted), lyraStringLength(inserted));
    std::string output;
    output.append(separator);
    for (std::size_t offset = 0; offset < input.size();) {
        auto width = lyra::utf8::sequenceWidth(static_cast<unsigned char>(input[offset]));
        output.append(input.substr(offset, width)).append(separator);
        offset += width;
    }
    return lyraStringFromBytes(output.data(), output.size());
}

extern "C" std::int64_t lyraTextParseFloat(const void* text, double* output) {
    auto first = lyraStringBytes(text);
    auto last = first + lyraStringLength(text);
    if (first != last && *first == '+') {
        ++first;
        if (first == last || *first == '-' || *first == '+') return 1;
    }
    auto [end, error] = std::from_chars(first, last, *output, std::chars_format::general);
    if (end != last || error == std::errc::invalid_argument) return 1;
    if (error == std::errc::result_out_of_range) return 2;
    return std::isfinite(*output) ? 0 : 1;
}

extern "C" const void* lyraBytesHex(const void* bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    auto size = lyraStringLength(bytes);
    std::string output(size * 2, '\0');
    for (std::size_t index = 0; index < size; ++index) {
        auto byte = lyraBytesAtByte(bytes, index);
        output[2 * index] = digits[byte >> 4];
        output[2 * index + 1] = digits[byte & 15];
    }
    return lyraStringFromBytes(output.data(), output.size());
}

extern "C" const void* lyraBytesFromHex(const void* text) {
    auto size = lyraStringLength(text);
    if (size % 2) return nullptr;
    auto* input = lyraStringBytes(text);
    auto digit = [](unsigned char byte) -> int {
        if ('0' <= byte && byte <= '9') return byte - '0';
        if ('a' <= byte && byte <= 'f') return byte - 'a' + 10;
        if ('A' <= byte && byte <= 'F') return byte - 'A' + 10;
        return -1;
    };
    std::string output(size / 2, '\0');
    for (std::size_t index = 0; index < output.size(); ++index) {
        auto high = digit(input[2 * index]), low = digit(input[2 * index + 1]);
        if (high < 0 || low < 0) return nullptr;
        output[index] = static_cast<char>(high * 16 + low);
    }
    return lyraStringFromBytes(output.data(), output.size());
}
