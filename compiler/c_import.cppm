module;
#include <algorithm>
#include <clang-c/Index.h>
#include <filesystem>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

export module lyra.c_import;
import lyra.platform;
import lyra.source;

namespace lyra {
namespace {

namespace fs = std::filesystem;
constexpr auto commandUsage = "lyra import-c <头文件> [-I <包含目录>] [-D <宏[=值]>] [-o <新文件.ly>]";

struct ImportOptions {
    fs::path header;
    std::optional<fs::path> output;
    std::vector<std::string> flags;
};

ImportOptions options(const std::vector<std::string_view>& args) {
    ImportOptions result;
    for (std::size_t index = 1; index < args.size(); ++index) {
        auto arg = args[index];
        auto operand = [&] {
            if (++index == args.size() || args[index].empty() || args[index].starts_with('-')) throw Diagnostic{"LYC0001", commandUsage};
            return args[index];
        };
        if (arg.starts_with("-I")) {
            auto path = fs::absolute(fromUtf8(arg.size() == 2 ? operand() : arg.substr(2)));
            if (!fs::is_directory(path)) throw Diagnostic{"LYI0001", "找不到 C 包含目录：" + pathText(path)};
            result.flags.push_back("-I" + pathText(path));
        } else if (arg.starts_with("-D")) {
            result.flags.push_back("-D" + std::string(arg.size() == 2 ? operand() : arg.substr(2)));
        } else if (arg == "-o" && !result.output) {
            result.output = fs::absolute(fromUtf8(operand()));
            if (result.output->extension() != ".ly") throw Diagnostic{"LYC0001", commandUsage};
        } else if (!arg.empty() && !arg.starts_with('-') && result.header.empty()) {
            result.header = fs::absolute(fromUtf8(arg));
        } else throw Diagnostic{"LYC0001", commandUsage};
    }
    if (result.header.empty()) throw Diagnostic{"LYC0001", commandUsage};
    if (!fs::is_regular_file(result.header)) throw Diagnostic{"LYI0001", "找不到 C 头文件：" + pathText(result.header)};
    return result;
}

struct ClangApi {
    llvm::sys::DynamicLibrary library;

    template<class Function> Function symbol(const char* name) {
        if (auto* address = library.getAddressOfSymbol(name)) return reinterpret_cast<Function>(address);
        throw Diagnostic{"LYF0003", "SDK 自带的 Clang 缺少接口：" + std::string(name)};
    }

#define LYRA_CLANG(function) decltype(&::function) function = symbol<decltype(&::function)>(#function)
    LYRA_CLANG(clang_createIndex);
    LYRA_CLANG(clang_disposeIndex);
    LYRA_CLANG(clang_parseTranslationUnit2);
    LYRA_CLANG(clang_disposeTranslationUnit);
    LYRA_CLANG(clang_getNumDiagnostics);
    LYRA_CLANG(clang_getDiagnostic);
    LYRA_CLANG(clang_disposeDiagnostic);
    LYRA_CLANG(clang_getDiagnosticSeverity);
    LYRA_CLANG(clang_getDiagnosticSpelling);
    LYRA_CLANG(clang_getDiagnosticLocation);
    LYRA_CLANG(clang_getTranslationUnitCursor);
    LYRA_CLANG(clang_visitChildren);
    LYRA_CLANG(clang_getCursorLocation);
    LYRA_CLANG(clang_getExpansionLocation);
    LYRA_CLANG(clang_Location_isFromMainFile);
    LYRA_CLANG(clang_getFileName);
    LYRA_CLANG(clang_getFileContents);
    LYRA_CLANG(clang_getCString);
    LYRA_CLANG(clang_disposeString);
    LYRA_CLANG(clang_getCursorSpelling);
    LYRA_CLANG(clang_Cursor_getMangling);
    LYRA_CLANG(clang_getCursorLinkage);
    LYRA_CLANG(clang_Cursor_isFunctionInlined);
    LYRA_CLANG(clang_getCursorType);
    LYRA_CLANG(clang_getCanonicalType);
    LYRA_CLANG(clang_getFunctionTypeCallingConv);
    LYRA_CLANG(clang_isFunctionTypeVariadic);
    LYRA_CLANG(clang_getResultType);
    LYRA_CLANG(clang_getArgType);
    LYRA_CLANG(clang_getNumArgTypes);
    LYRA_CLANG(clang_Cursor_getArgument);
    LYRA_CLANG(clang_getTypeSpelling);
    LYRA_CLANG(clang_Type_getSizeOf);
#undef LYRA_CLANG

    std::string text(CXString value) const {
        const auto* bytes = clang_getCString(value);
        std::string result = bytes ? bytes : "";
        clang_disposeString(value);
        return result;
    }
};

ClangApi loadClang(const fs::path& tools) {
    auto path = pathText(tools / (windows ? "libclang.dll" : "libclang.so"));
    std::string reason;
    auto library = llvm::sys::DynamicLibrary::getPermanentLibrary(path.c_str(), &reason);
    if (!library.isValid()) throw Diagnostic{"LYF0003", "无法加载 SDK 自带的 Clang：" + path + '\n' + reason};
    return {library};
}

bool identifier(std::string_view name) {
    auto letter = [](char value) { return value == '_' || (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z'); };
    return !name.empty() && letter(name.front()) && std::ranges::all_of(name, [&](char value) {
        return letter(value) || (value >= '0' && value <= '9');
    });
}

std::string escaped(std::string_view name) { return '`' + std::string(name) + '`'; }

struct Importer {
    const ClangApi& api;
    CXTranslationUnit unit;
    std::vector<Diagnostic> warnings{};
    std::set<std::string> emitted{};
    std::string bindings{};

    std::optional<Site> site(CXSourceLocation location, std::size_t width) const {
        CXFile file = nullptr;
        unsigned line = 0, column = 0, offset = 0;
        api.clang_getExpansionLocation(location, &file, &line, &column, &offset);
        if (!file || !line || !column) return {};
        auto path = pathText(fromUtf8(api.text(api.clang_getFileName(file))));
        std::size_t bytes = 0;
        const auto* buffer = api.clang_getFileContents(unit, file, &bytes);
        if (!buffer || offset > bytes || column > offset + 1) return Site{std::move(path), {offset, line, column}, {offset, line, column}, {}, offset};
        std::string_view text(buffer, bytes);
        auto lineStart = offset - (column - 1);
        auto lineEnd = std::min(text.find_first_of("\r\n", offset), text.size());
        auto countScalars = [](std::string_view part) {
            return static_cast<std::size_t>(std::ranges::count_if(part, [](unsigned char byte) { return (byte & 0xc0) != 0x80; }));
        };
        auto startColumn = countScalars(text.substr(lineStart, offset - lineStart)) + 1;
        auto end = offset + std::min(width, lineEnd - offset);
        auto endColumn = startColumn + countScalars(text.substr(offset, end - offset));
        return Site{std::move(path), {offset, line, startColumn}, {end, line, endColumn}, std::string(text.substr(lineStart, lineEnd - lineStart)), lineStart};
    }

    void clangDiagnostics() {
        for (unsigned next = 0; next < api.clang_getNumDiagnostics(unit); ++next) {
            std::unique_ptr<std::remove_pointer_t<CXDiagnostic>, decltype(api.clang_disposeDiagnostic)> diag(
                api.clang_getDiagnostic(unit, next), api.clang_disposeDiagnostic);
            auto severity = api.clang_getDiagnosticSeverity(diag.get());
            if (severity < CXDiagnostic_Warning) continue;
            auto location = site(api.clang_getDiagnosticLocation(diag.get()), 1);
            auto message = api.text(api.clang_getDiagnosticSpelling(diag.get()));
            if (severity >= CXDiagnostic_Error) throw Diagnostic{"LYF0003", "Clang：" + message, std::move(location)};
            warnings.push_back({"LYF0003", "Clang：" + message, std::move(location)});
        }
    }

    std::optional<std::string> typeName(CXType type, bool result = false) const {
        type = api.clang_getCanonicalType(type);
        switch (type.kind) {
            case CXType_Void: if (result) return "Unit"; return {};
            case CXType_Pointer: return "CPtr";
            case CXType_Bool: return "Bool";
            case CXType_Float: return "Float32";
            case CXType_Double: return "Float64";
            default: break;
        }
        auto width = api.clang_Type_getSizeOf(type);
        if (width != 1 && width != 2 && width != 4 && width != 8) return {};
        auto bits = std::to_string(width * 8);
        switch (type.kind) {
            case CXType_Char_S: case CXType_SChar: case CXType_Short: case CXType_Int: case CXType_Long: case CXType_LongLong:
                return "Int" + bits;
            case CXType_Char_U: case CXType_UChar: case CXType_UShort: case CXType_UInt: case CXType_ULong: case CXType_ULongLong:
                return "UInt" + bits;
            default: return {};
        }
    }

    void declaration(CXCursor cursor) {
        auto location = api.clang_getCursorLocation(cursor);
        if (!api.clang_Location_isFromMainFile(location)) return;
        auto name = api.text(api.clang_getCursorSpelling(cursor));
        auto skip = [&](std::string reason) {
            warnings.push_back({"LYF0004", "跳过 " + name + "：" + reason, site(location, name.size()), {}, {}, "可为此接口手写 extern 声明或提供 C 适配层"});
        };
        if (cursor.kind == CXCursor_VarDecl) {
            if (api.clang_getCursorLinkage(cursor) == CXLinkage_External) skip("不导入 C 全局变量");
            return;
        }
        if (api.clang_getCursorLinkage(cursor) != CXLinkage_External || api.clang_Cursor_isFunctionInlined(cursor))
            return skip("只导入非 inline 的外部 C 函数");
        if (!identifier(name) || name == "main" || name == "wmain" || name != api.text(api.clang_Cursor_getMangling(cursor)))
            return skip("C 符号不能直接映射为 Lyra 标识符");
        if (emitted.contains(name)) return;
        auto signature = api.clang_getCanonicalType(api.clang_getCursorType(cursor));
        auto convention = api.clang_getFunctionTypeCallingConv(signature);
        if (signature.kind != CXType_FunctionProto || api.clang_isFunctionTypeVariadic(signature) ||
            (convention != CXCallingConv_C && convention != (windows ? CXCallingConv_Win64 : CXCallingConv_X86_64SysV)))
            return skip("需要完整原型、定长参数和目标平台的默认 C 调用约定");
        auto result = api.clang_getResultType(signature);
        auto output = typeName(result, true);
        if (!output) return skip("不支持返回类型 " + api.text(api.clang_getTypeSpelling(result)));
        std::vector<std::string> parameters, parameterTypes;
        std::set<std::string> used;
        for (int index = 0, count = api.clang_getNumArgTypes(signature); index < count; ++index) {
            auto type = api.clang_getArgType(signature, static_cast<unsigned>(index));
            auto mapped = typeName(type);
            if (!mapped) return skip("不支持形参类型 " + api.text(api.clang_getTypeSpelling(type)));
            auto arg = api.text(api.clang_getCursorSpelling(api.clang_Cursor_getArgument(cursor, static_cast<unsigned>(index))));
            if (identifier(arg)) used.insert(arg);
            else arg.clear();
            parameters.push_back(std::move(arg));
            parameterTypes.push_back(std::move(*mapped));
        }
        bindings += "pub extern fn " + escaped(name) + '(';
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (index) bindings += ", ";
            auto& param = parameters[index];
            if (param.empty()) {
                param = "arg" + std::to_string(index + 1);
                while (!used.insert(param).second) param += '_';
            }
            bindings += escaped(param) + ": " + parameterTypes[index];
        }
        bindings += "): " + *output + ";\n";
        emitted.insert(std::move(name));
    }

    void run() {
        clangDiagnostics();
        std::vector<CXCursor> declarations;
        api.clang_visitChildren(api.clang_getTranslationUnitCursor(unit), [](CXCursor cursor, CXCursor, CXClientData state) {
            if (cursor.kind == CXCursor_FunctionDecl || cursor.kind == CXCursor_VarDecl)
                static_cast<std::vector<CXCursor>*>(state)->push_back(cursor);
            return CXChildVisit_Continue;
        }, &declarations);
        for (auto cursor : declarations) declaration(cursor);
    }
};

void emit(const ImportOptions& opts, const std::string& bindings) {
    if (!opts.output) { llvm::outs() << bindings; return; }
    std::error_code error;
    llvm::raw_fd_ostream stream(pathText(*opts.output), error, llvm::sys::fs::CD_CreateNew, llvm::sys::fs::FA_Write, llvm::sys::fs::OF_None);
    if (error) throw fs::filesystem_error("create C bindings", *opts.output, error);
    stream << bindings;
    stream.close();
    if (stream.has_error()) {
        auto reason = stream.error();
        stream.clear_error();
        throw fs::filesystem_error("write C bindings", *opts.output, reason);
    }
}

}

export int importC(const std::vector<std::string_view>& args, bool json) {
    auto opts = options(args);
    auto tools = sdkRoot() / "libexec/lyra";
    auto headers = tools / "clang/include";
    if (!fs::is_directory(headers)) throw Diagnostic{"LYF0003", "SDK 缺少 Clang 内建头文件：" + pathText(headers)};
    auto api = loadClang(tools);
    auto target = windows ? "--target=x86_64-w64-windows-gnu" : "--target=x86_64-pc-linux-gnu";
    std::vector<std::string> flags{"-x", "c", "-std=c17", target, "-ffreestanding", "-nostdinc", "-isystem", pathText(headers)};
    flags.insert(flags.end(), opts.flags.begin(), opts.flags.end());
    std::vector<const char*> arguments;
    for (const auto& flag : flags) arguments.push_back(flag.c_str());
    std::unique_ptr<std::remove_pointer_t<CXIndex>, decltype(api.clang_disposeIndex)> index(api.clang_createIndex(0, 0), api.clang_disposeIndex);
    auto header = pathText(opts.header);
    CXTranslationUnit raw = nullptr;
    auto status = api.clang_parseTranslationUnit2(index.get(), header.c_str(), arguments.data(), static_cast<int>(arguments.size()), nullptr, 0,
        CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_KeepGoing, &raw);
    if (status != CXError_Success) throw Diagnostic{"LYF0003", "Clang 无法解析头文件：" + header + "（错误码 " + std::to_string(status) + "）"};
    std::unique_ptr<std::remove_pointer_t<CXTranslationUnit>, decltype(api.clang_disposeTranslationUnit)> unit(raw, api.clang_disposeTranslationUnit);
    Importer importer{api, unit.get()};
    importer.run();
    emit(opts, importer.bindings);
    reportWarnings(importer.warnings, json);
    return 0;
}

}
