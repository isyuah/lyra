#include "io.h"
#include "managed.h"
#include "strings.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

struct LyraIoText {
    int status;
    const void* value;
};

namespace {

void* memory(void* value) {
    if (!value) { std::fputs("Lyra runtime: I/O buffer allocation failed\n", stderr); std::abort(); }
    return value;
}

std::FILE* openFile(const void* path, const char* mode) {
    auto length = lyraStringLength(path);
    auto data = lyraStringBytes(path);
    if (std::memchr(data, '\0', length)) { errno = EINVAL; return nullptr; }
    auto name = static_cast<char*>(memory(std::malloc(length + 1)));
    std::memcpy(name, data, length);
    name[length] = '\0';
    auto file = std::fopen(name, mode);
    auto error = errno;
    std::free(name);
    errno = error;
    return file;
}

int write(std::FILE* file, const void* content) {
    auto length = lyraStringLength(content);
    return std::fwrite(lyraStringBytes(content), 1, length, file) == length ? 0 : (errno ? errno : EIO);
}

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
const void* lyraIoTextValue(const LyraIoText* result) { return result->value; }

LyraIoText* lyraIoReadLine() {
    char* data = nullptr;
    std::size_t capacity = 0;
    auto count = ::getline(&data, &capacity, stdin);
    auto status = count >= 0 ? 0 : std::feof(stdin) ? -1 : errno ? errno : EIO;
    if (count > 0 && data[count - 1] == '\n') {
        --count;
        if (count && data[count - 1] == '\r') --count;
    }
    auto result = lyraIoDecode(data, count < 0 ? 0 : static_cast<std::size_t>(count), status);
    std::free(data);
    return result;
}

LyraIoText* lyraIoReadFile(const void* path) {
    auto file = openFile(path, "rb");
    if (!file) return lyraIoDecode(nullptr, 0, errno);
    char buffer[8192];
    char* data = nullptr;
    std::size_t length = 0, capacity = 0;
    int status = 0;
    while (auto count = std::fread(buffer, 1, sizeof(buffer), file)) {
        constexpr auto limit = std::numeric_limits<std::size_t>::max();
        if (length > limit - count) { status = EOVERFLOW; break; }
        if (length + count > capacity) {
            capacity = capacity > limit / 2 || capacity * 2 < length + count ? length + count : capacity * 2;
            data = static_cast<char*>(memory(std::realloc(data, capacity)));
        }
        std::memcpy(data + length, buffer, count);
        length += count;
    }
    if (!status && std::ferror(file)) status = errno ? errno : EIO;
    if (std::fclose(file) && !status) status = errno;
    auto result = lyraIoDecode(data, length, status);
    std::free(data);
    return result;
}

int64_t lyraIoWriteFile(const void* path, const void* content) {
    auto file = openFile(path, "wb");
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
