#include "text_io.h"
#include "strings.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <uv.h>
#include <vector>

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

extern "C" LyraIoText* lyraOsEnv(const void* key) {
    std::string name(lyraStringBytes(key), lyraStringLength(key));
    if (name.empty() || name.find_first_of("=\0", 0, 2) != name.npos) return lyraIoDecode(nullptr, 0, -4);
    char initial[256];
    std::size_t size = sizeof(initial);
    auto status = uv_os_getenv(name.c_str(), initial, &size);
    if (!status) return lyraIoDecode(initial, size, 0);
    std::vector<char> buffer;
    while (status == UV_ENOBUFS) {
        buffer.resize(size);
        status = uv_os_getenv(name.c_str(), buffer.data(), &size);
    }
    if (!status) return lyraIoDecode(buffer.data(), size, 0);
    return lyraIoDecode(nullptr, 0, status == UV_ENOENT ? -1 : status == UV_EINVAL ? -4 : -status);
}

extern "C" uint8_t lyraOsWindows() {
#ifdef _WIN32
    return 1;
#else
    return 0;
#endif
}
