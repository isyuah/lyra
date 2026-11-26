module;
#include <algorithm>
#include <clang-c/Index.h>
#include <filesystem>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/raw_ostream.h>
#include <map>
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
constexpr auto commandUsage = "lyra import-c <头文件> [-I <包含目录>] [-D <宏[=值]>] [-o <新文件.ly> [--adapter <新文件.c> [--prefix <符号前缀>] [--varargs 'C名=Lyra名(Int32,Float64,…)']...]]";

bool identifier(std::string_view name) {
    auto letter = [](char value) { return value == '_' || (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z'); };
    return !name.empty() && letter(name.front()) && std::ranges::all_of(name, [&](char value) {
        return letter(value) || (value >= '0' && value <= '9');
    });
}

std::string scalarC(std::string_view type) {
    if (type == "Bool") return "_Bool";
    if (type == "CPtr") return "void *";
    if (type == "Float32") return "float";
    if (type == "Float64") return "double";
    if (type == "Unit") return "void";
    auto width = type.starts_with("UInt") ? type.substr(4) : type.starts_with("Int") ? type.substr(3) : std::string_view{};
    if (width != "8" && width != "16" && width != "32" && width != "64") return {};
    return std::string(type.starts_with('U') ? "uint" : "int") + std::string(width) + "_t";
}

struct VariadicShape {
    std::string symbol, name;
    std::vector<std::string> types;
    bool emitted{};
};

VariadicShape variadicShape(std::string_view input) {
    auto equals = input.find('=');
    auto open = input.find('(');
    if (equals == input.npos || open == input.npos || equals >= open || !input.ends_with(')'))
        throw Diagnostic{"LYC0001", commandUsage};
    auto symbol = input.substr(0, equals), name = input.substr(equals + 1, open - equals - 1);
    if (!identifier(symbol) || !identifier(name) || name == "main") throw Diagnostic{"LYC0001", commandUsage};
    VariadicShape result{std::string(symbol), std::string(name), {}, false};
    auto types = input.substr(open + 1, input.size() - open - 2);
    while (!types.empty()) {
        auto comma = types.find(',');
        auto next = types.substr(0, comma);
        if (next == "Unit" || scalarC(next).empty() || (comma != types.npos && comma + 1 == types.size()))
            throw Diagnostic{"LYC0001", "--varargs 需要用逗号分隔的定宽数值、Bool 或 CPtr 类型：" + std::string(input)};
        result.types.emplace_back(next);
        if (comma == types.npos) break;
        types.remove_prefix(comma + 1);
    }
    return result;
}

struct ImportOptions {
    fs::path header;
    std::optional<fs::path> output, adapter;
    std::vector<std::string> flags;
    std::string prefix;
    std::vector<VariadicShape> variadics;
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
        } else if (arg == "--adapter" && !result.adapter) {
            result.adapter = fs::absolute(fromUtf8(operand()));
            if (result.adapter->extension() != ".c") throw Diagnostic{"LYC0001", commandUsage};
        } else if (arg == "--prefix" && result.prefix.empty()) {
            result.prefix = operand();
            if (!identifier(result.prefix)) throw Diagnostic{"LYC0001", commandUsage};
        } else if (arg == "--varargs") {
            auto shape = variadicShape(operand());
            if (std::ranges::find(result.variadics, shape.name, &VariadicShape::name) != result.variadics.end())
                throw Diagnostic{"LYC0001", "--varargs 的 Lyra 函数名重复：" + shape.name};
            result.variadics.push_back(std::move(shape));
        } else if (!arg.empty() && !arg.starts_with('-') && result.header.empty()) {
            result.header = fs::absolute(fromUtf8(arg));
        } else throw Diagnostic{"LYC0001", commandUsage};
    }
    if (result.header.empty()) throw Diagnostic{"LYC0001", commandUsage};
    if ((result.adapter && !result.output) || (!result.adapter && (!result.prefix.empty() || !result.variadics.empty())))
        throw Diagnostic{"LYC0001", commandUsage};
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
    LYRA_CLANG(clang_Cursor_isBitField);
    LYRA_CLANG(clang_getCursorType);
    LYRA_CLANG(clang_getCursorDefinition);
    LYRA_CLANG(clang_getCursorUSR);
    LYRA_CLANG(clang_getCanonicalType);
    LYRA_CLANG(clang_getUnqualifiedType);
    LYRA_CLANG(clang_getTypeDeclaration);
    LYRA_CLANG(clang_isConstQualifiedType);
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

std::string escaped(std::string_view name) { return '`' + std::string(name) + '`'; }

struct CRecord;
struct MappedType {
    std::string lyra, bridge, original;
    const CRecord* record{};
    std::string cArgument(const std::string& name) const {
        if (record) return "*(const " + original + " *)" + name;
        return lyra == "CPtr" && !original.empty() ? "(" + original + ")" + name : name;
    }
    std::string cResult(const std::string& value) const { return lyra == "CPtr" ? "(void *)(" + value + ')' : value; }
};

struct CParameter { std::string name; MappedType type; };
struct CRecord { std::string name, native, symbol; std::vector<CParameter> fields; };

struct Adapter {
    std::string prefix, source, bindings;
    std::map<std::string, CRecord> records;
    std::set<std::string> recordNames;
    std::size_t serial{};

    explicit Adapter(const ImportOptions& opts) {
        auto include = pathText(opts.header.lexically_relative(opts.adapter->parent_path()));
        if (include.empty() || include.find_first_of("\"\n\r") != include.npos)
            throw Diagnostic{"LYF0003", "C 适配器需要可按相对路径包含的头文件"};
        auto header = llvm::MemoryBuffer::getFile(pathText(opts.header));
        if (!header) throw fs::filesystem_error("read C header", opts.header, header.getError());
        llvm::SHA256 digest;
        digest.update((*header)->getBuffer());
        digest.update(include);
        digest.update(pathText(opts.adapter->filename()));
        for (const auto& flag : opts.flags) if (flag.starts_with("-D")) {
            std::string_view macro(flag.data() + 2, flag.size() - 2);
            auto equals = macro.find('=');
            auto name = macro.substr(0, equals);
            auto value = equals == macro.npos ? std::string_view("1") : macro.substr(equals + 1);
            if (!identifier(name) || value.find_first_of("\r\n") != value.npos || value.ends_with('\\'))
                throw Diagnostic{"LYF0003", "无法在 C 适配器中重现宏定义：" + flag};
            digest.update(flag);
            source += "#define " + std::string(name) + ' ' + std::string(value) + '\n';
        }
        source += "#include \"" + include + "\"\n#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n";
        prefix = "lyraC_" + (opts.prefix.empty() ? llvm::toHex(digest.final(), true).substr(0, 20) : opts.prefix) + '_';
    }

    std::string symbol() { return prefix + std::to_string(++serial); }

    void record(const CRecord& type) {
        const auto& key = type.symbol;
        bindings += "pub struct " + type.name + '(';
        std::string initializer, packC = "void " + key + "Pack(void *destination", packLy = "extern fn " + key + "Pack(destination: CPtr";
        std::string readers, readBindings;
        for (std::size_t index = 0; index < type.fields.size(); ++index) {
            const auto& field = type.fields[index];
            if (index) { bindings += ", "; initializer += ", "; }
            const auto incoming = "field" + std::to_string(index);
            bindings += "pub val " + escaped(field.name) + ": " + field.type.lyra;
            initializer += '.' + field.name + " = " + field.type.cArgument(incoming);
            packC += ", " + field.type.bridge + ' ' + incoming;
            packLy += ", " + incoming + ": " + field.type.lyra;
            auto read = key + "Read" + std::to_string(index);
            readers += field.type.bridge + ' ' + read + "(const void *source) { return " +
                field.type.cResult("((const " + type.native + " *)source)->" + field.name) + "; }\n";
            readBindings += "extern fn " + read + "(source: CPtr): " + field.type.lyra + ";\n";
            source += "_Static_assert(sizeof(((" + type.native + " *)0)->" + field.name + ") == sizeof(" + field.type.bridge +
                "), \"C field width differs; regenerate the bindings for this target\");\n";
        }
        bindings += ")\nextern fn " + key + "Size(): Int;\n" + packLy + "): Unit;\n" + readBindings;
        source += "_Static_assert(_Alignof(" + type.native + ") <= _Alignof(max_align_t), \"C record alignment exceeds malloc alignment\");\n";
        source += "int64_t " + key + "Size(void) { return sizeof(" + type.native + "); }\n";
        source += packC + ") { " + type.native + " packed = { " + initializer + " }; memcpy(destination, &packed, sizeof packed); }\n" + readers;
    }

    void callable(std::string cExpression, const std::string& publicName, const std::vector<CParameter>& params, const MappedType& result) {
        auto entry = symbol();
        std::string cParams, lyParams, publicParams, cArgs, lyArgs, prologue;
        std::set<std::string> names;
        for (const auto& param : params) names.insert(param.name);
        auto fresh = [&](std::string name) {
            while (!names.insert(name).second) name += '_';
            return name;
        };
        for (std::size_t index = 0; index < params.size(); ++index) {
            const auto& param = params[index];
            auto input = "input" + std::to_string(index);
            auto name = escaped(param.name);
            if (index) { cParams += ", "; lyParams += ", "; publicParams += ", "; cArgs += ", "; lyArgs += ", "; }
            cParams += param.type.bridge + ' ' + input;
            lyParams += input + ": " + (param.type.record ? "CPtr" : param.type.lyra);
            publicParams += name + ": " + param.type.lyra;
            cArgs += param.type.cArgument(input);
            if (const auto* type = param.type.record) {
                auto storage = fresh("c" + input);
                prologue += "val " + storage + " = CPtr.alloc(" + type->symbol + "Size()); defer " + storage + ".free(); " +
                    type->symbol + "Pack(" + storage;
                for (const auto& field : type->fields) prologue += ", " + name + '.' + escaped(field.name);
                prologue += "); ";
                lyArgs += storage;
            } else lyArgs += name;
        }
        if (auto marker = cExpression.find('$'); marker != cExpression.npos) cExpression.replace(marker, 1, cArgs);
        std::string cBody, lyBody, cResult = result.bridge, lyResult = result.lyra;
        if (const auto* type = result.record) {
            auto storage = fresh("cOutput");
            if (!params.empty()) { cParams += ", "; lyParams += ", "; lyArgs += ", "; }
            cParams += "void *output";
            lyParams += "output: CPtr";
            lyArgs += storage;
            cResult = "void";
            lyResult = "Unit";
            cBody = result.original + " value = " + cExpression + "; memcpy(output, &value, sizeof value);";
            prologue += "val " + storage + " = CPtr.alloc(" + type->symbol + "Size()); defer " + storage + ".free(); ";
            lyBody = entry + '(' + lyArgs + "); " + type->name + '(';
            for (std::size_t index = 0; index < type->fields.size(); ++index) {
                if (index) lyBody += ", ";
                lyBody += type->symbol + "Read" + std::to_string(index) + '(' + storage + ')';
            }
            lyBody += ')';
        } else {
            cBody = std::string(result.lyra == "Unit" ? "" : "return ") + result.cResult(cExpression) + ';';
            lyBody = entry + '(' + lyArgs + ')';
        }
        source += cResult + ' ' + entry + '(' + (cParams.empty() ? "void" : cParams) + ") { " + cBody + " }\n";
        bindings += "extern fn " + entry + '(' + lyParams + "): " + lyResult + ";\npub fn " + escaped(publicName) + '(' + publicParams + "): " + result.lyra + " { ";
        bindings += prologue.empty() ? lyBody : "unsafe { " + prologue + lyBody + " }";
        bindings += " }\n";
    }
};

struct Importer {
    const ClangApi& api;
    CXTranslationUnit unit;
    Adapter* adapter;
    std::vector<VariadicShape>& variadics;
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

    std::optional<MappedType> scalar(CXType type, bool result = false) const {
        auto name = typeName(type, result);
        if (!name) return {};
        auto spelling = *name == "CPtr" ? api.text(api.clang_getTypeSpelling(api.clang_getUnqualifiedType(type))) : std::string{};
        return MappedType{*name, scalarC(*name), std::move(spelling)};
    }

    std::optional<MappedType> mapped(CXType type, bool result = false) {
        if (auto basic = scalar(type, result)) return basic;
        if (!adapter) return {};
        type = api.clang_getUnqualifiedType(type);
        auto canonical = api.clang_getCanonicalType(type);
        if (canonical.kind != CXType_Record) return {};
        auto decl = api.clang_getCursorDefinition(api.clang_getTypeDeclaration(canonical));
        if (decl.kind != CXCursor_StructDecl) return {};
        auto identity = api.text(api.clang_getCursorUSR(decl));
        if (auto known = adapter->records.find(identity); known != adapter->records.end()) {
            const auto& record = known->second;
            return MappedType{record.name, "const void *", record.native, &record};
        }
        auto native = api.text(api.clang_getTypeSpelling(type));
        std::string_view label(native);
        if (label.starts_with("struct ")) label.remove_prefix(7);
        if (!identifier(label)) return {};
        std::vector<CXCursor> children;
        api.clang_visitChildren(decl, [](CXCursor field, CXCursor, CXClientData data) {
            if (field.kind == CXCursor_FieldDecl) static_cast<std::vector<CXCursor>*>(data)->push_back(field);
            return CXChildVisit_Continue;
        }, &children);
        if (children.empty()) return {};
        std::vector<CParameter> fields;
        for (auto field : children) {
            auto name = api.text(api.clang_getCursorSpelling(field));
            auto member = scalar(api.clang_getCursorType(field));
            if (!identifier(name) || !member || api.clang_Cursor_isBitField(field)) return {};
            fields.push_back({std::move(name), std::move(*member)});
        }
        auto lyraName = "CRecord" + std::string(label);
        while (!adapter->recordNames.insert(lyraName).second) lyraName += '_';
        const auto& record = adapter->records.emplace(std::move(identity),
            CRecord{std::move(lyraName), std::move(native), adapter->symbol(), std::move(fields)}).first->second;
        adapter->record(record);
        return MappedType{record.name, "const void *", record.native, &record};
    }

    void declaration(CXCursor cursor) {
        auto location = api.clang_getCursorLocation(cursor);
        if (!api.clang_Location_isFromMainFile(location)) return;
        auto name = api.text(api.clang_getCursorSpelling(cursor));
        auto skip = [&](std::string reason) {
            warnings.push_back({"LYF0004", "跳过 " + name + "：" + reason, site(location, name.size()), {}, {}, "可用 --adapter 生成支持的 C 适配层，或手写绑定"});
        };
        auto requireAvailableName = [&] {
            if (std::ranges::any_of(variadics, [&](const auto& shape) { return shape.name == name; }))
                throw Diagnostic{"LYF0003", "--varargs 的 Lyra 名称与头文件声明冲突：" + name, site(location, name.size())};
        };
        if (emitted.contains(name)) return;
        const auto linkage = api.clang_getCursorLinkage(cursor);
        if (cursor.kind == CXCursor_VarDecl) {
            if (linkage != CXLinkage_External) return;
            if (!adapter) return skip("C 全局变量需要 --adapter");
            auto type = api.clang_getCursorType(cursor);
            auto value = scalar(type);
            if (!identifier(name) || !value || name == "main") return skip("只适配标量或指针全局变量");
            requireAvailableName();
            adapter->callable(name, name, {}, *value);
            if (!api.clang_isConstQualifiedType(api.clang_getCanonicalType(type)))
                adapter->callable(name + " = $", name, {{"value", *value}}, {"Unit", "void", {}});
            emitted.insert(name);
            return;
        }
        const bool inlined = api.clang_Cursor_isFunctionInlined(cursor);
        const bool localInline = inlined && linkage == CXLinkage_Internal && api.clang_getCursorDefinition(cursor).kind == CXCursor_FunctionDecl;
        if ((!inlined && linkage != CXLinkage_External) || (inlined && !(adapter && localInline)))
            return skip("只接受外部函数，或用 --adapter 导入有定义的 static inline");
        if (!identifier(name) || name == "main" || name == "wmain" || (!adapter && name != api.text(api.clang_Cursor_getMangling(cursor))))
            return skip("C 符号不能直接映射为 Lyra 标识符");
        auto signature = api.clang_getCanonicalType(api.clang_getCursorType(cursor));
        auto convention = api.clang_getFunctionTypeCallingConv(signature);
        if (signature.kind != CXType_FunctionProto || (convention != CXCallingConv_C && convention != (windows ? CXCallingConv_Win64 : CXCallingConv_X86_64SysV)))
            return skip("需要完整原型和目标平台的默认 C 调用约定");
        const bool variadic = api.clang_isFunctionTypeVariadic(signature);
        if (variadic && std::ranges::none_of(variadics, [&](const auto& shape) { return shape.symbol == name; }))
            return skip("可变参数函数需要用 --varargs 声明调用形状");
        auto result = api.clang_getResultType(api.clang_getCursorType(cursor));
        auto output = mapped(result, true);
        if (!output) return skip("不支持返回类型 " + api.text(api.clang_getTypeSpelling(result)));
        std::vector<CParameter> parameters;
        std::set<std::string> used;
        for (int index = 0, count = api.clang_getNumArgTypes(signature); index < count; ++index) {
            const auto slot = static_cast<unsigned>(index);
            auto type = api.clang_getArgType(api.clang_getCursorType(cursor), slot);
            auto param = mapped(type);
            if (!param) return skip("不支持形参类型 " + api.text(api.clang_getTypeSpelling(type)));
            auto arg = api.text(api.clang_getCursorSpelling(api.clang_Cursor_getArgument(cursor, slot)));
            if (identifier(arg)) used.insert(arg);
            else arg.clear();
            parameters.push_back({std::move(arg), std::move(*param)});
        }
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            auto& param = parameters[index].name;
            if (param.empty()) {
                param = "arg" + std::to_string(index + 1);
                while (!used.insert(param).second) param += '_';
            }
        }
        if (variadic) {
            for (auto& shape : variadics) if (shape.symbol == name && !shape.emitted) {
                auto fixed = parameters;
                auto names = used;
                for (const auto& type : shape.types) {
                    auto parameter = "arg" + std::to_string(fixed.size() + 1);
                    while (!names.insert(parameter).second) parameter += '_';
                    fixed.push_back({std::move(parameter), {type, scalarC(type), {}}});
                }
                adapter->callable(name + "($)", shape.name, fixed, *output);
                shape.emitted = true;
            }
        } else {
            requireAvailableName();
            if (adapter && (inlined || output->record || name != api.text(api.clang_Cursor_getMangling(cursor)) ||
                std::ranges::any_of(parameters, [](const auto& item) { return item.type.record; }))) {
                adapter->callable(name + "($)", name, parameters, *output);
            } else {
                bindings += "pub extern fn " + escaped(name) + '(';
                for (std::size_t index = 0; index < parameters.size(); ++index) {
                    if (index) bindings += ", ";
                    bindings += escaped(parameters[index].name) + ": " + parameters[index].type.lyra;
                }
                bindings += "): " + output->lyra + ";\n";
            }
        }
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
        for (const auto& shape : variadics) if (!shape.emitted)
            throw Diagnostic{"LYF0003", "找不到可适配的可变参数函数：" + shape.symbol};
    }
};

void emit(const fs::path& path, const std::string& source) {
    std::error_code error;
    llvm::raw_fd_ostream stream(pathText(path), error, llvm::sys::fs::CD_CreateNew, llvm::sys::fs::FA_Write, llvm::sys::fs::OF_None);
    if (error) throw fs::filesystem_error("create C bindings", path, error);
    stream << source;
    stream.close();
    if (stream.has_error()) {
        auto reason = stream.error();
        stream.clear_error();
        fs::remove(path, error);
        throw fs::filesystem_error("write C bindings", path, reason);
    }
}

}

export int importC(const std::vector<std::string_view>& args, bool json) {
    auto opts = options(args);
    auto tools = sdkRoot() / "libexec/lyra";
    auto headers = tools / "clang/include";
    if (!fs::is_directory(headers)) throw Diagnostic{"LYF0003", "SDK 缺少 Clang 内建头文件：" + pathText(headers)};
    auto api = loadClang(tools);
    auto flags = nativeCFlags();
    flags.insert(flags.end(), opts.flags.begin(), opts.flags.end());
    std::vector<const char*> arguments;
    for (const auto& flag : flags) arguments.push_back(flag.c_str());
    std::unique_ptr<std::remove_pointer_t<CXIndex>, decltype(api.clang_disposeIndex)> index(api.clang_createIndex(0, 0), api.clang_disposeIndex);
    auto header = pathText(opts.header);
    CXTranslationUnit raw = nullptr;
    unsigned parseFlags = CXTranslationUnit_KeepGoing;
    if (!opts.adapter) parseFlags |= CXTranslationUnit_SkipFunctionBodies;
    auto status = api.clang_parseTranslationUnit2(index.get(), header.c_str(), arguments.data(), static_cast<int>(arguments.size()), nullptr, 0, parseFlags, &raw);
    if (status != CXError_Success) throw Diagnostic{"LYF0003", "Clang 无法解析头文件：" + header + "（错误码 " + std::to_string(status) + "）"};
    std::unique_ptr<std::remove_pointer_t<CXTranslationUnit>, decltype(api.clang_disposeTranslationUnit)> unit(raw, api.clang_disposeTranslationUnit);
    auto adapter = opts.adapter ? std::optional<Adapter>(std::in_place, opts) : std::nullopt;
    Importer importer{api, unit.get(), adapter ? &*adapter : nullptr, opts.variadics};
    importer.run();
    if (adapter) {
        emit(*opts.output, importer.bindings + adapter->bindings);
        try { emit(*opts.adapter, adapter->source); }
        catch (...) {
            std::error_code ignored;
            fs::remove(*opts.output, ignored);
            throw;
        }
    } else if (opts.output) emit(*opts.output, importer.bindings);
    else llvm::outs() << importer.bindings;
    reportWarnings(importer.warnings, json);
    return 0;
}

}
