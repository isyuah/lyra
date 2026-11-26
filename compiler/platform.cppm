module;
#include <cstdlib>
#include <filesystem>
#include <llvm/Support/FileSystem.h>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <windows.h>
#endif

export module lyra.platform;

export namespace lyra {

#ifdef _WIN32
inline constexpr bool windows = true;
inline constexpr auto nativeTriple = "x86_64-pc-windows-msvc";
inline constexpr auto nativeEntry = "wmain";
#else
inline constexpr bool windows = false;
inline constexpr auto nativeTriple = "x86_64-pc-linux-gnu";
inline constexpr auto nativeEntry = "main";
#endif
inline constexpr std::string_view linuxLoader = "/lib64/ld-linux-x86-64.so.2";

std::filesystem::path fromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string pathText(const std::filesystem::path& path) {
    auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

const std::filesystem::path& sdkRoot() {
    static const auto root = [] {
#ifdef _WIN32
        auto executable = fromUtf8(llvm::sys::fs::getMainExecutable(nullptr, nullptr));
#else
        auto executable = std::filesystem::path("/proc/self/exe");
#endif
        return std::filesystem::canonical(executable).parent_path().parent_path();
    }();
    return root;
}

std::filesystem::path executablePath(std::filesystem::path path) {
    if constexpr (windows) {
        if (!path.has_extension()) path += ".exe";
    }
    return path;
}

std::filesystem::path cacheOverride() {
#ifdef _WIN32
    auto length = GetEnvironmentVariableW(L"LYRA_CACHE", nullptr, 0);
    if (!length) return {};
    std::wstring value(length, L'\0');
    auto written = GetEnvironmentVariableW(L"LYRA_CACHE", value.data(), length);
    if (!written || written >= length) return {};
    value.resize(written);
    return value;
#else
    auto value = std::getenv("LYRA_CACHE");
    return value ? std::filesystem::path(value) : std::filesystem::path{};
#endif
}

void publishExecutable(const std::filesystem::path& staged, const std::filesystem::path& output, [[maybe_unused]] bool replace = true) {
#ifdef _WIN32
    auto name = output;
    name.make_preferred();
    auto bytes = static_cast<DWORD>(name.native().size() * sizeof(wchar_t));
    auto size = static_cast<DWORD>(sizeof(FILE_RENAME_INFO)) + bytes;
    auto buffer = std::make_unique<std::byte[]>(size);
    auto* rename = ::new (buffer.get()) FILE_RENAME_INFO{};
    constexpr DWORD replaceExisting = 0x1, posixSemantics = 0x2;
    rename->Flags = replace ? replaceExisting | posixSemantics : 0;
    rename->FileNameLength = bytes;
    std::memcpy(buffer.get() + offsetof(FILE_RENAME_INFO, FileName), name.c_str(), bytes);
    auto handle = CreateFileW(staged.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::filesystem::filesystem_error("open staged executable", staged, std::error_code(GetLastError(), std::system_category()));
    auto renamed = SetFileInformationByHandle(handle, FileRenameInfoEx, rename, size);
    auto status = renamed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (status == ERROR_ACCESS_DENIED || status == ERROR_SHARING_VIOLATION)
        throw std::filesystem::filesystem_error("无法替换可执行文件；若仍在运行，请先退出或指定另一输出路径", staged, output, std::error_code(status, std::system_category()));
    if (status != ERROR_SUCCESS)
        throw std::filesystem::filesystem_error("replace executable", staged, output, std::error_code(status, std::system_category()));
#else
    std::filesystem::rename(staged, output);
#endif
}

class StagingDirectory {
public:
    explicit StagingDirectory(const std::filesystem::path& parent) {
        std::filesystem::create_directories(parent);
        llvm::SmallString<128> temporary;
        auto error = llvm::sys::fs::createUniqueDirectory(pathText(parent / ".lyra-tmp"), temporary);
        if (error) throw std::filesystem::filesystem_error("create temporary directory", parent, error);
        path = fromUtf8(temporary.str().str());
    }
    StagingDirectory(const StagingDirectory&) = delete;
    StagingDirectory& operator=(const StagingDirectory&) = delete;
    ~StagingDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

}
