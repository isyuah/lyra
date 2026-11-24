#include "text_io.h"
#include "locks.h"
#include "managed.h"
#include "strings.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

#ifdef _WIN32
#include <windows.h>
#endif

struct LyraIoText {
    int status;
    const void* value;
};

struct LyraIoBytes {
    int status;
    const void* value;
};

struct LyraIoFile {
    std::atomic_flag busy{};
    std::FILE* stream;
    bool writing;
};

struct LyraIoFileOpen {
    int status;
    LyraIoFile* file;
};

namespace {

enum FileStatus : int { fileEof = -1, fileClosed = -2, fileBusy = -3, fileInvalid = -4 };

struct FileLease {
    explicit FileLease(LyraIoFile& file) : file(file), held(!file.busy.test_and_set(std::memory_order_acquire)) {}
    ~FileLease() { if (held) file.busy.clear(std::memory_order_release); }
    int access(bool writing) const {
        if (!held) return fileBusy;
        if (!file.stream) return fileClosed;
        return file.writing == writing ? 0 : fileInvalid;
    }
    LyraIoFile& file;
    bool held;
};

int systemStatus() { return errno ? errno : EIO; }

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
    errno = 0;
    auto file = std::fopen(name, writing ? "wb" : "rb");
    auto error = errno;
#endif
    std::free(name);
    errno = error;
    return file;
}

int write(std::FILE* file, const void* content) {
    auto length = lyraStringLength(content);
    errno = 0;
    return std::fwrite(lyraStringBytes(content), 1, length, file) == length ? 0 : systemStatus();
}

int closeFile(std::FILE* file) {
    errno = 0;
    return std::fclose(file) ? systemStatus() : 0;
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

int readAll(const void* path, ByteBuffer& data) {
    auto file = openFile(path, false);
    if (!file) return systemStatus();
    char buffer[8192];
    int status = 0;
    errno = 0;
    while (auto count = std::fread(buffer, 1, sizeof(buffer), file)) {
        if (!data.append(buffer, count)) { status = EOVERFLOW; break; }
        if (std::ferror(file)) break;
        errno = 0;
    }
    if (!status && std::ferror(file)) status = systemStatus();
    auto closed = closeFile(file);
    return status ? status : closed;
}

int writeAll(const void* path, const void* content) {
    auto file = openFile(path, true);
    if (!file) return systemStatus();
    auto status = write(file, content);
    auto closed = closeFile(file);
    return status ? status : closed;
}

LyraIoBytes* bytes(const char* data, std::size_t size, int status) {
    auto out = static_cast<LyraIoBytes*>(lyraGcAlloc(sizeof(LyraIoBytes)));
    *out = {status, status ? nullptr : lyraStringFromBytes(data, size)};
    return out;
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
uint8_t lyraIoInvalidUtf8(int64_t status) { return status == EILSEQ; }
const void* lyraIoTextValue(const LyraIoText* result) { return result->value; }

LyraIoText* lyraIoReadLine() {
    static uv_once_t initialized = UV_ONCE_INIT;
    static uv_mutex_t input;
    uv_once(&initialized, [] { lyraRuntime::initMutex(input); });
    lyraRuntime::Lock lock(input);
    ByteBuffer line;
    for (;;) {
        errno = 0;
        auto next = std::fgetc(stdin);
        if (next == EOF) {
            auto status = std::ferror(stdin) ? systemStatus() : line.length ? 0 : fileEof;
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
    ByteBuffer data;
    auto status = readAll(path, data);
    return lyraIoDecode(data.data, data.length, status);
}

int64_t lyraIoWriteFile(const void* path, const void* content) { return writeAll(path, content); }
int64_t lyraIoWriteBytes(const void* path, const void* content) { return writeAll(path, content); }

LyraIoBytes* lyraIoReadBytes(const void* path) {
    ByteBuffer data;
    auto status = readAll(path, data);
    return bytes(data.data, data.length, status);
}

int64_t lyraIoBytesStatus(const LyraIoBytes* result) { return result->status; }
const void* lyraIoBytesValue(const LyraIoBytes* result) { return result->value; }

LyraIoFileOpen* lyraIoFileOpen(const void* path, uint8_t writing) {
    auto stream = openFile(path, writing);
    auto status = stream ? 0 : systemStatus();
    auto result = static_cast<LyraIoFileOpen*>(lyraGcAlloc(sizeof(LyraIoFileOpen)));
    *result = {status, stream ? ::new (lyraGcAlloc(sizeof(LyraIoFile))) LyraIoFile{{}, stream, writing != 0} : nullptr};
    return result;
}

int64_t lyraIoFileOpenStatus(const LyraIoFileOpen* result) { return result->status; }
LyraIoFile* lyraIoFileOpened(const LyraIoFileOpen* result) { return result->file; }

LyraIoBytes* lyraIoFileRead(LyraIoFile* file, int64_t maximum) {
    FileLease lease(*file);
    if (auto status = lease.access(false)) return bytes(nullptr, 0, status);
    if (maximum <= 0) return bytes(nullptr, 0, fileInvalid);
    char buffer[65536];
    auto limit = maximum < static_cast<int64_t>(sizeof(buffer)) ? static_cast<std::size_t>(maximum) : sizeof(buffer);
    errno = 0;
    auto count = std::fread(buffer, 1, limit, file->stream);
    if (std::ferror(file->stream)) return bytes(nullptr, 0, systemStatus());
    return bytes(buffer, count, count ? 0 : fileEof);
}

int64_t lyraIoFileWrite(LyraIoFile* file, const void* content) {
    FileLease lease(*file);
    auto status = lease.access(true);
    return status ? status : write(file->stream, content);
}

int64_t lyraIoFileFlush(LyraIoFile* file) {
    FileLease lease(*file);
    if (auto status = lease.access(true)) return status;
    errno = 0;
    return std::fflush(file->stream) ? systemStatus() : 0;
}

int64_t lyraIoFileClose(LyraIoFile* file) {
    FileLease lease(*file);
    if (!lease.held) return fileBusy;
    auto stream = file->stream;
    file->stream = nullptr;
    return stream ? closeFile(stream) : 0;
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
