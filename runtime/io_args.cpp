#include "text_io.h"

#include <cerrno>
#include <cstddef>
#include <cstring>

#ifdef _WIN32
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

LyraIoText* lyraIoDecode(const char* data, std::size_t size, int status);

namespace {
#ifdef _WIN32
using ArgumentChar = wchar_t;
#else
using ArgumentChar = char;
#endif
int argumentCount;
ArgumentChar** arguments;
}

void lyraIoSetArgs(int argc, ArgumentChar** argv) {
    argumentCount = argc;
    arguments = argv;
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
}

int64_t lyraIoArgCount() {
    return argumentCount - 1;
}

LyraIoText* lyraIoArgument(int64_t index) {
    if (index < 0 || index >= lyraIoArgCount()) return lyraIoDecode(nullptr, 0, ERANGE);
    auto value = arguments[index + 1];
#ifdef _WIN32
    auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (!length) return lyraIoDecode(nullptr, 0, EILSEQ);
    auto bytes = static_cast<char*>(std::malloc(length));
    if (!bytes) std::abort();
    auto converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, bytes, length, nullptr, nullptr);
    auto result = lyraIoDecode(bytes, converted ? converted - 1 : 0, converted ? 0 : EILSEQ);
    std::free(bytes);
    return result;
#else
    return lyraIoDecode(value, std::strlen(value), 0);
#endif
}
