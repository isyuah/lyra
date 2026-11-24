#include "backtrace.h"
#include "managed.h"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#ifdef _WIN32
#include "locks.h"
#include <windows.h>
#include <dbghelp.h>
#include <cwchar>
#include <iterator>
#else
#include <dlfcn.h>
#include <execinfo.h>
#endif

namespace lyraRuntime {

struct Backtrace {
    static constexpr unsigned limit = 32;
    unsigned size;
    void* addresses[limit];
};

namespace {

void frame(FILE* stream, const char* module, const void* base, const void* address, const char* symbol) {
    if (!base) { std::fprintf(stream, "  native %p\n", address); return; }
    if (auto separator = std::strrchr(module, '/')) module = separator + 1;
    if (auto separator = std::strrchr(module, '\\')) module = separator + 1;
    std::fprintf(stream, "  native %s+0x%" PRIxPTR, module,
        reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(base));
    if (symbol && *symbol) std::fprintf(stream, " (%s)", symbol);
    std::fputc('\n', stream);
}

#ifdef _WIN32
struct DebugSymbols {
    static inline uv_once_t initialized = UV_ONCE_INIT;
    static inline uv_mutex_t mutex;
    HANDLE process = nullptr;
    bool ready = false;

    DebugSymbols() {
        auto current = GetCurrentProcess();
        if (!DuplicateHandle(current, current, current, &process, 0, FALSE, DUPLICATE_SAME_ACCESS)) return;
        wchar_t directory[32768];
        auto length = GetModuleFileNameW(nullptr, directory, static_cast<DWORD>(std::size(directory)));
        if (length == 0 || length == std::size(directory)) return;
        auto* separator = std::wcsrchr(directory, L'\\');
        if (!separator) return;
        separator[1] = L'\0';
        ready = SymInitializeW(process, directory, TRUE) != FALSE;
    }

    ~DebugSymbols() {
        if (ready) SymCleanup(process);
        if (process) CloseHandle(process);
    }

    void write(FILE* stream, const void* address) const {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<const wchar_t*>(address), &module)) {
            frame(stream, nullptr, nullptr, address, nullptr);
            return;
        }
        wchar_t path[32768];
        auto length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
        char name[1024];
        int nameLength = 0;
        if (length && length < std::size(path)) {
            auto* separator = std::wcsrchr(path, L'\\');
            nameLength = WideCharToMultiByte(CP_UTF8, 0, separator ? separator + 1 : path, -1, name, sizeof name, nullptr, nullptr);
        }
        alignas(SYMBOL_INFO) std::byte buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
        auto* symbol = ::new (buffer) SYMBOL_INFO{};
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement;
        auto found = ready && SymFromAddr(process, reinterpret_cast<DWORD64>(address), &displacement, symbol);
        frame(stream, nameLength ? name : "?", module, address, found ? symbol->Name : nullptr);
    }
};
#endif

}

[[clang::noinline]] const Backtrace* captureBacktrace() {
#ifdef _WIN32
    wchar_t setting[2];
    if (GetEnvironmentVariableW(L"LYRA_NATIVE_BACKTRACE", setting, 2) != 1 || setting[0] != L'1') return nullptr;
#else
    auto* setting = std::getenv("LYRA_NATIVE_BACKTRACE");
    if (!setting || std::strcmp(setting, "1") != 0) return nullptr;
#endif
    Backtrace trace{};
#ifdef _WIN32
    trace.size = CaptureStackBackTrace(2, Backtrace::limit, trace.addresses, nullptr);
#else
    void* addresses[Backtrace::limit + 2];
    auto captured = ::backtrace(addresses, Backtrace::limit + 2);
    if (captured <= 2) return nullptr;
    trace.size = static_cast<unsigned>(captured - 2);
    std::memcpy(trace.addresses, addresses + 2, trace.size * sizeof(void*));
#endif
    if (!trace.size) return nullptr;
    return ::new (lyraGcAllocAtomic(sizeof(Backtrace))) Backtrace(trace);
}

void writeBacktrace(const Backtrace& trace, FILE* stream) {
#ifdef _WIN32
    uv_once(&DebugSymbols::initialized, [] { initMutex(DebugSymbols::mutex); });
    Lock guard(DebugSymbols::mutex);
    DebugSymbols symbols;
#endif
    for (unsigned index = 0; index < trace.size; ++index) {
        const auto* address = static_cast<const char*>(trace.addresses[index]) - 1;
#ifdef _WIN32
        symbols.write(stream, address);
#else
        Dl_info module{};
        dladdr(address, &module);
        frame(stream, module.dli_fname, module.dli_fbase, address, module.dli_sname);
#endif
    }
}

}
