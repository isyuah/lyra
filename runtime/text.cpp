#include "strings.h"
#include "managed.h"
#include <lyra/utf8.hpp>

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
