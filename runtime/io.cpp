#include "text_io.h"
#include "locks.h"
#include "managed.h"
#include "strings.h"

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#ifdef _WIN32
#include <windows.h>
#endif

struct LyraIoText {
    int status;
    const void* value;
};

namespace {

void* memory(void* value) {
    if (!value) { std::fputs("Lyra runtime: I/O buffer allocation failed\n", stderr); std::abort(); }
    return value;
}

std::FILE* openFile(const void* path, bool writing) {
    auto length = lyraStringLength(path);
    auto data = lyraStringBytes(path);
    if (std::memchr(data, '\0', length)) { errno = EINVAL; return nullptr; }
#ifdef _WIN32
    if (length == 0) { errno = ENOENT; return nullptr; }
    if (length > INT_MAX) { errno = EOVERFLOW; return nullptr; }
    auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(length), nullptr, 0);
    if (!count) { errno = EILSEQ; return nullptr; }
    auto name = static_cast<wchar_t*>(memory(std::malloc((static_cast<std::size_t>(count) + 1) * sizeof(wchar_t))));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(length), name, count);
    name[count] = L'\0';
    std::FILE* file;
    auto error = _wfopen_s(&file, name, writing ? L"wb" : L"rb");
#else
    auto name = static_cast<char*>(memory(std::malloc(length + 1)));
    std::memcpy(name, data, length);
    name[length] = '\0';
    auto file = std::fopen(name, writing ? "wb" : "rb");
    auto error = errno;
#endif
    std::free(name);
    errno = error;
    return file;
}

int write(std::FILE* file, const void* content) {
    auto length = lyraStringLength(content);
    return std::fwrite(lyraStringBytes(content), 1, length, file) == length ? 0 : (errno ? errno : EIO);
}

struct ByteBuffer {
    char* data{};
    std::size_t length{}, capacity{};

    ~ByteBuffer() { std::free(data); }

    bool append(const char* bytes, std::size_t size) {
        constexpr auto limit = std::numeric_limits<std::size_t>::max();
        if (size > limit - length) return false;
        auto needed = length + size;
        if (needed > capacity) {
            auto growth = capacity ? (capacity > limit / 2 ? limit : capacity * 2) : 128;
            capacity = growth < needed ? needed : growth;
            data = static_cast<char*>(memory(std::realloc(data, capacity)));
        }
        std::memcpy(data + length, bytes, size);
        length = needed;
        return true;
    }
};

}

LyraIoText* lyraIoDecode(const char* data, std::size_t size, int status) {
    if (status == 0 && !lyraUtf8Valid(data, size)) status = EILSEQ;
    auto value = lyraStringFromBytes(data, status == 0 ? size : 0);
    auto out = static_cast<LyraIoText*>(lyraGcAlloc(sizeof(LyraIoText)));
    out->status = status;
    out->value = value;
    return out;
}

int64_t lyraIoTextStatus(const LyraIoText* result) { return result->status; }
uint8_t lyraIoInvalidUtf8(int64_t status) { return status == EILSEQ; }
const void* lyraIoTextValue(const LyraIoText* result) { return result->value; }

LyraIoText* lyraIoReadLine() {
    static uv_once_t initialized = UV_ONCE_INIT;
    static uv_mutex_t input;
    uv_once(&initialized, [] { lyraRuntime::initMutex(input); });
    lyraRuntime::Lock lock(input);
    ByteBuffer line;
    for (;;) {
        auto next = std::fgetc(stdin);
        if (next == EOF) {
            auto status = std::ferror(stdin) ? (errno ? errno : EIO) : line.length ? 0 : -1;
            return lyraIoDecode(line.data, line.length, status);
        }
        if (next == '\n') {
            if (line.length && line.data[line.length - 1] == '\r') --line.length;
            return lyraIoDecode(line.data, line.length, 0);
        }
        auto byte = static_cast<char>(next);
        if (!line.append(&byte, 1)) return lyraIoDecode(nullptr, 0, EOVERFLOW);
    }
}

LyraIoText* lyraIoReadFile(const void* path) {
    auto file = openFile(path, false);
    if (!file) return lyraIoDecode(nullptr, 0, errno);
    char buffer[8192];
    ByteBuffer data;
    int status = 0;
    while (auto count = std::fread(buffer, 1, sizeof(buffer), file)) {
        if (!data.append(buffer, count)) { status = EOVERFLOW; break; }
    }
    if (!status && std::ferror(file)) status = errno ? errno : EIO;
    if (std::fclose(file) && !status) status = errno;
    return lyraIoDecode(data.data, data.length, status);
}

int64_t lyraIoWriteFile(const void* path, const void* content) {
    auto file = openFile(path, true);
    if (!file) return errno;
    auto status = write(file, content);
    if (std::fclose(file) && !status) status = errno;
    return status;
}

int64_t lyraIoWriteError(const void* content) {
    auto status = write(stderr, content);
    if (std::fflush(stderr) && !status) status = errno;
    return status;
}

void lyraIoAssert(uint8_t condition, const void* message) {
    if (condition) return;
    std::fputs("assertion failed: ", stderr);
    write(stderr, message);
    std::fputc('\n', stderr);
    std::exit(EXIT_FAILURE);
}
