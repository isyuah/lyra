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
#include <string_view>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
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

struct LyraIoPosition {
    int status;
    int64_t offset;
};

struct LyraFsDirectory {
    int status;
    int64_t count;
    const void** names;
};

struct LyraFsTempFile {
    int status;
    const void* path;
    LyraIoFile* file;
};

LyraIoText* lyraIoDecode(const char* data, std::size_t size, int status);

namespace {

enum FileStatus : int { fileEof = -1, fileClosed = -2, fileBusy = -3, fileInvalid = -4, fileInvalidUtf8 = -5, fileLineTooLong = -6 };
enum class FileMode : int64_t { Read, Write, Append };

struct FileLease {
    explicit FileLease(LyraIoFile& file) : file(file), held(!file.busy.test_and_set(std::memory_order_acquire)) {}
    ~FileLease() { if (held) file.busy.clear(std::memory_order_release); }
    int access() const {
        if (!held) return fileBusy;
        if (!file.stream) return fileClosed;
        return 0;
    }
    int access(bool writing) const {
        if (auto status = access()) return status;
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

std::FILE* openFile(const void* path, FileMode mode) {
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
    auto error = _wfopen_s(&file, name, mode == FileMode::Read ? L"rb" : mode == FileMode::Write ? L"wb" : L"ab");
#else
    auto name = static_cast<char*>(memory(std::malloc(length + 1)));
    std::memcpy(name, data, length);
    name[length] = '\0';
    errno = 0;
    auto file = std::fopen(name, mode == FileMode::Read ? "rb" : mode == FileMode::Write ? "wb" : "ab");
    auto error = errno;
#endif
    std::free(name);
    errno = error;
    return file;
}

template <typename Operation>
int streamOutput(Operation perform) {
#ifndef _WIN32
    sigset_t blocked, previous, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &previous) || sigpending(&pending)) std::abort();
    const bool alreadyPending = sigismember(&pending, SIGPIPE) == 1;
#endif
    const auto status = perform();
#ifndef _WIN32
    if (status == EPIPE && !alreadyPending) {
        constexpr timespec immediate{};
        while (sigtimedwait(&blocked, nullptr, &immediate) == -1 && errno == EINTR) {}
    }
    if (pthread_sigmask(SIG_SETMASK, &previous, nullptr)) std::abort();
#endif
    return status;
}

int write(std::FILE* file, const void* content) {
    return streamOutput([&] {
        const auto length = lyraStringLength(content);
        errno = 0;
        const auto count = std::fwrite(lyraStringBytes(content), 1, length, file);
        return count < length || (errno && std::ferror(file)) ? systemStatus() : 0;
    });
}

int flushFile(std::FILE* file) {
    return streamOutput([&] {
        errno = 0;
        return std::fflush(file) ? systemStatus() : 0;
    });
}

int closeFile(std::FILE* file) {
    return streamOutput([&] {
        errno = 0;
        return std::fclose(file) ? systemStatus() : 0;
    });
}

struct ByteBuffer {
    char* data{};
    std::size_t length{}, capacity{};

    ~ByteBuffer() { std::free(data); }

    bool append(const char* bytes, std::size_t size, std::size_t limit = std::numeric_limits<std::size_t>::max()) {
        if (!size) return true;
        if (size > limit - length) return false;
        auto needed = length + size;
        if (needed > capacity) {
            auto growth = capacity > limit / 2 ? limit : capacity * 2;
            if (!capacity) growth = limit < 128 ? limit : 128;
            capacity = growth < needed ? needed : growth;
            data = static_cast<char*>(memory(std::realloc(data, capacity)));
        }
        std::memcpy(data + length, bytes, size);
        length = needed;
        return true;
    }
};

using Path = std::string_view;

Path pathView(const void* path) { return {lyraStringBytes(path), lyraStringLength(path)}; }
bool validPath(Path path) { return !path.empty() && path.find('\0') == Path::npos; }

#ifdef _WIN32
constexpr char pathSeparator = '\\';
bool separator(char byte) { return byte == '\\' || byte == '/'; }
char asciiLower(char byte) { return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a') : byte; }
bool drive(Path path) { return path.size() >= 2 && path[1] == ':' && asciiLower(path[0]) >= 'a' && asciiLower(path[0]) <= 'z'; }
#else
constexpr char pathSeparator = '/';
bool separator(char byte) { return byte == '/'; }
#endif

std::size_t skipSeparators(Path path, std::size_t start) {
    while (start < path.size() && separator(path[start])) ++start;
    return start;
}

std::size_t componentEnd(Path path, std::size_t start) {
    while (start < path.size() && !separator(path[start])) ++start;
    return start;
}

struct PathRoot { std::size_t name, first; bool absolute; };

PathRoot pathRoot(Path path) {
    std::size_t name = 0;
#ifdef _WIN32
    if (drive(path)) name = 2;
    else if (path.size() >= 3 && separator(path[0]) && separator(path[1]) && !separator(path[2])) {
        const auto server = componentEnd(path, 2);
        const auto share = skipSeparators(path, server);
        name = share == path.size() ? server : componentEnd(path, share);
        if (server == 3 && path[2] == '?' && name - share == 3 &&
            asciiLower(path[share]) == 'u' && asciiLower(path[share + 1]) == 'n' && asciiLower(path[share + 2]) == 'c') {
            const auto extendedServer = componentEnd(path, skipSeparators(path, name));
            name = componentEnd(path, skipSeparators(path, extendedServer));
        }
    }
#endif
    const auto first = skipSeparators(path, name);
    return {name, first, first != name || name > 2};
}

bool appendPath(ByteBuffer& result, Path directory, Path name) {
    auto needsSeparator = !directory.empty() && !separator(directory.back());
#ifdef _WIN32
    if (directory.size() == 2 && drive(directory)) needsSeparator = false;
#endif
    return result.append(directory.data(), directory.size()) &&
        (!needsSeparator || result.append(&pathSeparator, 1)) && result.append(name.data(), name.size());
}

struct FsRequest {
    uv_fs_t request{};
    ~FsRequest() { uv_fs_req_cleanup(&request); }
};

int fsStatus(int status) { return status < 0 ? -status : 0; }

int removePath(const char* path) {
    FsRequest remove;
    return fsStatus(uv_fs_unlink(nullptr, &remove.request, path, nullptr));
}

int ensureDirectory(const char* path) {
    FsRequest info;
    auto status = uv_fs_stat(nullptr, &info.request, path, nullptr);
    if (status < 0) return fsStatus(status);
    return (info.request.statbuf.st_mode & S_IFMT) == S_IFDIR ? 0 : fsStatus(UV_ENOTDIR);
}

struct TempStream { int status; const void* path; std::FILE* stream; };

TempStream openTemp(Path directory, Path prefix) {
    if (!validPath(directory) || prefix.find_first_of("/\\:\0", 0, 4) != Path::npos) return {fileInvalid, nullptr, nullptr};
    ByteBuffer name;
    if (!appendPath(name, directory, prefix) || !name.append("XXXXXX", sizeof("XXXXXX"))) return {EOVERFLOW, nullptr, nullptr};
    FsRequest create;
    const auto descriptor = uv_fs_mkstemp(nullptr, &create.request, name.data, nullptr);
    if (descriptor < 0) return {fsStatus(descriptor), nullptr, nullptr};
    const auto path = uv_fs_get_path(&create.request);
    errno = 0;
#ifdef _WIN32
    auto* stream = _fdopen(descriptor, "wb");
#else
    auto* stream = fdopen(descriptor, "wb");
#endif
    if (!stream) {
        auto status = systemStatus();
        FsRequest close;
        uv_fs_close(nullptr, &close.request, descriptor, nullptr);
        removePath(path);
        return {status, nullptr, nullptr};
    }
    return {0, lyraStringFromBytes(path, std::strlen(path)), stream};
}

LyraIoText* readLine(std::FILE* stream, int64_t maximum) {
    if (maximum <= 0 || static_cast<uint64_t>(maximum) >= std::numeric_limits<std::size_t>::max())
        return lyraIoDecode(nullptr, 0, fileInvalid);
    const auto limit = static_cast<std::size_t>(maximum);
    ByteBuffer line;
    bool overflow = false;
    for (;;) {
        errno = 0;
        const auto next = std::fgetc(stream);
        if (next == EOF) {
            if (std::ferror(stream)) return lyraIoDecode(nullptr, 0, systemStatus());
            if (overflow || line.length > limit) return lyraIoDecode(nullptr, 0, fileLineTooLong);
            return lyraIoDecode(line.data, line.length, line.length ? 0 : fileEof);
        }
        if (next == '\n') {
            if (line.length && line.data[line.length - 1] == '\r') --line.length;
            if (overflow || line.length > limit) return lyraIoDecode(nullptr, 0, fileLineTooLong);
            return lyraIoDecode(line.data, line.length, 0);
        }
        const auto byte = static_cast<char>(next);
        if (!overflow) overflow = !line.append(&byte, 1, limit + 1);
    }
}

int readAll(const void* path, ByteBuffer& data) {
    if (!validPath(pathView(path))) return fileInvalid;
    auto file = openFile(path, FileMode::Read);
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
    if (!validPath(pathView(path))) return fileInvalid;
    auto file = openFile(path, FileMode::Write);
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

LyraIoFile* wrapFile(std::FILE* stream, bool writing) {
    return ::new (lyraGcAlloc(sizeof(LyraIoFile))) LyraIoFile{{}, stream, writing};
}

LyraIoPosition* position(int status, int64_t offset = 0) {
    auto* result = static_cast<LyraIoPosition*>(lyraGcAlloc(sizeof(LyraIoPosition)));
    *result = {status, offset};
    return result;
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

LyraIoText* lyraIoReadLine(int64_t maximum) {
    static uv_once_t initialized = UV_ONCE_INIT;
    static uv_mutex_t input;
    uv_once(&initialized, [] { lyraRuntime::initMutex(input); });
    lyraRuntime::Lock lock(input);
    return readLine(stdin, maximum);
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

LyraIoFileOpen* lyraIoFileOpen(const void* path, int64_t mode) {
    auto operation = static_cast<FileMode>(mode);
    const auto valid = validPath(pathView(path));
    auto stream = valid ? openFile(path, operation) : nullptr;
    auto status = stream ? 0 : valid ? systemStatus() : fileInvalid;
    auto result = static_cast<LyraIoFileOpen*>(lyraGcAlloc(sizeof(LyraIoFileOpen)));
    *result = {status, stream ? wrapFile(stream, operation != FileMode::Read) : nullptr};
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

extern "C" LyraIoText* lyraIoFileReadLine(LyraIoFile* file, int64_t maximum) {
    FileLease lease(*file);
    if (auto status = lease.access(false)) return lyraIoDecode(nullptr, 0, status);
    return readLine(file->stream, maximum);
}

extern "C" LyraIoPosition* lyraIoFileSeek(LyraIoFile* file, int64_t offset, int64_t origin) {
    FileLease lease(*file);
    if (auto status = lease.access()) return position(status);
    const auto from = origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END;
    int64_t current{};
    const auto status = streamOutput([&] {
        errno = 0;
#ifdef _WIN32
        if (_fseeki64(file->stream, offset, from) != 0) return systemStatus();
        current = _ftelli64(file->stream);
#else
        static_assert(sizeof(off_t) >= sizeof(int64_t));
        if (fseeko(file->stream, offset, from) != 0) return systemStatus();
        current = ftello(file->stream);
#endif
        return current < 0 ? systemStatus() : 0;
    });
    return position(status, current);
}

extern "C" int64_t lyraIoPositionStatus(const LyraIoPosition* result) { return result->status; }
extern "C" int64_t lyraIoPositionValue(const LyraIoPosition* result) { return result->offset; }

int64_t lyraIoFileWrite(LyraIoFile* file, const void* content) {
    FileLease lease(*file);
    auto status = lease.access(true);
    return status ? status : write(file->stream, content);
}

int64_t lyraIoFileFlush(LyraIoFile* file) {
    FileLease lease(*file);
    if (auto status = lease.access(true)) return status;
    return flushFile(file->stream);
}

int64_t lyraIoFileClose(LyraIoFile* file) {
    FileLease lease(*file);
    if (!lease.held) return fileBusy;
    auto stream = file->stream;
    file->stream = nullptr;
    return stream ? closeFile(stream) : 0;
}

extern "C" const void* lyraFsPathJoin(const void* base, const void* segment) {
    const auto directory = pathView(base);
    const auto name = pathView(segment);
    if (name.empty()) return base;
    if (directory.empty()) return segment;
    const auto root = pathRoot(name);
    ByteBuffer result;
#ifdef _WIN32
    const auto parent = pathRoot(directory);
    if (root.name && (root.absolute || parent.name != root.name || asciiLower(directory[0]) != asciiLower(name[0])))
        return segment;
    if (root.absolute) {
        if (!parent.name) return segment;
        if (!result.append(directory.data(), parent.name) || !result.append(name.data(), name.size())) std::abort();
        return lyraStringFromBytes(result.data, result.length);
    }
#else
    if (root.absolute) return segment;
#endif
    if (root.name == name.size()) return base;
    if (!appendPath(result, directory, Path{name.data() + root.name, name.size() - root.name})) std::abort();
    return lyraStringFromBytes(result.data, result.length);
}

extern "C" LyraFsDirectory* lyraFsListDir(const void* path) {
    auto* result = static_cast<LyraFsDirectory*>(lyraGcAlloc(sizeof(LyraFsDirectory)));
    *result = {};
    const auto source = pathView(path);
    if (!validPath(source)) { result->status = fileInvalid; return result; }
    ByteBuffer name;
    if (!name.append(source.data(), source.size()) || !name.append("", 1)) { result->status = EOVERFLOW; return result; }
    FsRequest scan;
    const auto count = uv_fs_scandir(nullptr, &scan.request, name.data, 0, nullptr);
    if (count < 0) { result->status = fsStatus(count); return result; }
    if (count == 0) return result;
    result->names = static_cast<const void**>(lyraGcAlloc(static_cast<std::size_t>(count) * sizeof(*result->names)));
    for (int index = 0; index < count; ++index) {
        uv_dirent_t entry;
        if (const auto status = uv_fs_scandir_next(&scan.request, &entry); status < 0) {
            result->status = status == UV_EOF ? EIO : fsStatus(status);
            return result;
        }
        const auto length = std::strlen(entry.name);
        if (!lyraUtf8Valid(entry.name, length)) { result->status = fileInvalidUtf8; return result; }
        result->names[index] = lyraStringFromBytes(entry.name, length);
    }
    result->count = count;
    return result;
}

extern "C" int64_t lyraFsDirStatus(const LyraFsDirectory* directory) { return directory->status; }
extern "C" int64_t lyraFsDirCount(const LyraFsDirectory* directory) { return directory->count; }
extern "C" const void* lyraFsDirName(const LyraFsDirectory* directory, int64_t index) { return directory->names[index]; }

extern "C" int64_t lyraFsMkdirAll(const void* path) {
    const auto source = pathView(path);
    if (!validPath(source)) return fileInvalid;
    ByteBuffer name;
    if (!name.append(source.data(), source.size()) || !name.append("", 1)) return EOVERFLOW;
    auto begin = pathRoot(source).first;
    if (begin == source.size()) return ensureDirectory(name.data);
    while (begin < source.size()) {
        const auto end = componentEnd(source, begin);
        const auto next = name.data[end];
        name.data[end] = '\0';
        FsRequest create;
        const auto status = uv_fs_mkdir(nullptr, &create.request, name.data, 0777, nullptr);
        if (status < 0) {
            if (status != UV_EEXIST) return fsStatus(status);
            if (const auto exists = ensureDirectory(name.data)) return exists;
        }
        name.data[end] = next;
        begin = skipSeparators(source, end);
    }
    return 0;
}

extern "C" int64_t lyraFsRemoveFile(const void* path) {
    const auto source = pathView(path);
    if (!validPath(source)) return fileInvalid;
    ByteBuffer name;
    if (!name.append(source.data(), source.size()) || !name.append("", 1)) return EOVERFLOW;
    return removePath(name.data);
}

extern "C" LyraFsTempFile* lyraFsCreateTempFile(const void* directory, const void* prefix) {
    const auto temp = openTemp(pathView(directory), pathView(prefix));
    auto* result = static_cast<LyraFsTempFile*>(lyraGcAlloc(sizeof(LyraFsTempFile)));
    *result = {temp.status, temp.path, temp.stream ? wrapFile(temp.stream, true) : nullptr};
    return result;
}

extern "C" int64_t lyraFsTempStatus(const LyraFsTempFile* temp) { return temp->status; }
extern "C" const void* lyraFsTempPath(const LyraFsTempFile* temp) { return temp->path; }
extern "C" LyraIoFile* lyraFsTempHandle(const LyraFsTempFile* temp) { return temp->file; }

extern "C" int64_t lyraFsAtomicWrite(const void* path, const void* content) {
    const auto target = pathView(path);
    if (!validPath(target) || separator(target.back())) return fileInvalid;
    const auto root = pathRoot(target);
    auto parentEnd = target.size();
    while (parentEnd > root.first && !separator(target[parentEnd - 1])) --parentEnd;
    if (root.first == target.size()) return fileInvalid;
    const auto temporary = openTemp(parentEnd ? Path{target.data(), parentEnd} : Path{"."}, ".lyra-");
    if (temporary.status) return temporary.status;
    ByteBuffer sourceName, targetName;
    const auto source = pathView(temporary.path);
    if (!sourceName.append(source.data(), source.size()) || !sourceName.append("", 1) ||
        !targetName.append(target.data(), target.size()) || !targetName.append("", 1)) std::abort();
    auto status = write(temporary.stream, content);
    const auto closed = closeFile(temporary.stream);
    if (!status) status = closed;
    if (!status) {
        FsRequest replace;
        status = fsStatus(uv_fs_rename(nullptr, &replace.request, sourceName.data, targetName.data, nullptr));
    }
    if (status) removePath(sourceName.data);
    return status;
}

int64_t lyraIoWriteError(const void* content) {
    if (auto status = write(stderr, content)) return status;
    return flushFile(stderr);
}
