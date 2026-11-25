module;
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/SHA256.h>

export module lyra.macros;
import lyra.check;
import lyra.macro_syntax;
import lyra.numbers;
import lyra.platform;

export namespace lyra {
inline constexpr std::size_t macroInputByteLimit = 1024 * 1024;
using MacroFiles = std::map<std::string, std::map<std::string, std::filesystem::path>>;
struct MacroPackage { std::filesystem::path root; MacroFiles inputs; const SourceOverlay* overlays{}; std::filesystem::path cache{}; };
struct MacroType { std::string kind, name, module; std::vector<MacroType> arguments{}; };
using MacroTypeResolver = std::function<MacroType(std::size_t, const Identifier&, std::span<const Identifier>)>;
void expandMacros(Module& syntax, std::vector<Source>& sources, const std::map<std::string, MacroPackage>& packages,
    std::vector<std::filesystem::path>& inputs, const MacroTypeResolver& resolveType, std::string_view typeEnvironment);
void parsePackageFile(Module& syntax, Source& source, std::size_t file, const MacroPackage& package, std::vector<std::filesystem::path>& inputs);
}

namespace lyra {
namespace fs = std::filesystem;
namespace {
namespace json = llvm::json;
using Value = std::variant<std::monostate, bool, std::int64_t, std::string, SyntaxRef>;
struct Returned { Value value; };
constexpr auto byteLimit = macroInputByteLimit;
constexpr std::size_t cacheByteLimit = 16 * byteLimit;
constexpr std::size_t checksumLength = 64;
constexpr auto prelude = R"(struct Syntax(val handle: Int)
fn fieldCount(): Int { 0 }
fn fieldName(index: Int): String { "" }
fn fieldType(index: Int): String { "" }
fn fieldPublic(index: Int): Bool { false }
fn fieldDefaultKind(index: Int): String { "" }
fn fieldDefaultFunction(index: Int): String { "" }
fn fieldHasAttribute(index: Int, name: String): Bool { false }
fn fieldAttributeValue(index: Int, name: String): String { "" }
fn argumentCount(): Int { 0 }
fn argumentString(index: Int): String { "" }
fn targetKind(): String { "" }
fn typeParameterCount(): Int { 0 }
fn typeParameterName(index: Int): String { "" }
fn typeParameterDeclarations(): String { "" }
fn typeParameterArguments(): String { "" }
fn syntaxTarget(): Syntax { Syntax(0) }
fn syntaxBody(): Syntax { Syntax(0) }
fn syntaxArgument(index: Int): Syntax { Syntax(index) }
fn syntaxChildCount(node: Syntax): Int { 0 }
fn syntaxChild(node: Syntax, index: Int): Syntax { node }
fn syntaxKind(node: Syntax): String { "" }
fn syntaxText(node: Syntax): String { "" }
fn syntaxIdentifier(node: Syntax): String { "" }
fn syntaxStart(node: Syntax): Int { 0 }
fn syntaxEnd(node: Syntax): Int { 0 }
fn syntaxLine(node: Syntax): Int { 0 }
fn syntaxColumn(node: Syntax): Int { 0 }
fn syntaxCode(text: String): Syntax { Syntax(0) }
fn syntaxCodeAt(node: Syntax, text: String): Syntax { node }
fn syntaxResolvedType(node: Syntax): Syntax { node }
fn syntaxTypeModule(node: Syntax): String { "" }
fn syntaxString(text: String): Syntax { Syntax(0) }
fn syntaxInt(number: Int): Syntax { Syntax(0) }
fn syntaxName(text: String): Syntax { Syntax(0) }
fn syntaxJoin(left: Syntax, right: Syntax): Syntax { left }
fn syntaxConcat(left: Syntax, right: Syntax): Syntax { left }
fn syntaxArgs(left: Syntax, right: Syntax): Syntax { left }
fn syntaxCall(callee: Syntax, arguments: Syntax): Syntax { callee }
fn syntaxMember(receiver: Syntax, name: String): Syntax { receiver }
fn syntaxBlock(statements: Syntax): Syntax { statements }
fn syntaxAround(before: Syntax, after: Syntax, failure: Syntax): Syntax { before }
fn syntaxError(node: Syntax, message: String): Syntax { node }
fn optionInner(typeName: String): String { "" }
fn listElement(typeName: String): String { "" }
fn macroQuote(text: String): String { "" }
fn macroContains(text: String, fragment: String): Bool { false }
fn macroInput(key: String): String { "" }
fn macroFail(message: String): String { "" }
)";

struct ExpandedMacro {
    SyntaxOutput output;
    std::set<std::size_t> defaultFields;
    std::vector<std::string> inputKeys;
    fs::path pendingCache{};
};

struct MacroInput { fs::path path; std::string text; };
using InputSnapshot = std::map<std::string, MacroInput>;

std::string fieldDefaultReference(std::size_t field) {
    return "__lyraFieldDefault" + std::to_string(field);
}

MacroInput readInput(const Source& source, const Expansion& prefix, const MacroPackage& package, const std::string& key, const fs::path& grant) {
    auto path = package.root / grant;
    auto relative = fs::weakly_canonical(path).lexically_relative(fs::canonical(package.root));
    auto overlaid = package.overlays && package.overlays->contains(path);
    if (relative.empty() || *relative.begin() == ".." || (!overlaid && !fs::is_regular_file(path)) ||
        (overlaid ? package.overlays->at(path).size() : fs::file_size(path)) > byteLimit)
        throw at(source, prefix.name.span, "LYM0002", "只读宏输入不存在、越界或过大：" + key);
    auto text = loadSource(path, {}, overlaid ? package.overlays : nullptr).text;
    if (text.size() > byteLimit) throw at(source, prefix.name.span, "LYM0002", "只读宏输入不存在、越界或过大：" + key);
    return {std::move(path), std::move(text)};
}

std::optional<InputSnapshot> snapshotInputs(const Source& source, const Expansion& prefix, const MacroPackage& package) {
    InputSnapshot inputs;
    std::size_t size = 0;
    try {
        if (auto grants = package.inputs.find(prefix.name.text); grants != package.inputs.end()) {
            for (const auto& [key, grant] : grants->second) {
                auto input = readInput(source, prefix, package, key, grant);
                size += input.text.size();
                if (size > cacheByteLimit) return {};
                inputs.emplace(key, std::move(input));
            }
        }
    } catch (const Diagnostic&) { return {}; }
    catch (const fs::filesystem_error&) { return {}; }
    return inputs;
}

std::string checksum(std::string_view key, std::string_view text) {
    llvm::SHA256 hash;
    hash.update(key);
    hash.update(text);
    return llvm::toHex(hash.final(), true);
}

const std::string& compilerFingerprint() {
    static const auto fingerprint = [] {
        auto path = windows ? fromUtf8(llvm::sys::fs::getMainExecutable(nullptr, nullptr)) : fs::path("/proc/self/exe");
        std::ifstream input(path, std::ios::binary);
        llvm::SHA256 hash;
        char buffer[32 * 1024];
        while (input.read(buffer, sizeof buffer) || input.gcount() > 0)
            hash.update(llvm::StringRef(buffer, static_cast<std::size_t>(input.gcount())));
        return input.eof() && !input.bad() ? llvm::toHex(hash.final(), true) : std::string{};
    }();
    return fingerprint;
}

std::string cacheKey(const Source& source, const Expansion& prefix, const Source& code, const Identifier* target,
                     std::string_view kind, const InputSnapshot& inputs, std::string_view typeEnvironment) {
    llvm::SHA256 hash;
    auto text = [&](std::string_view value) {
        hash.update(std::to_string(value.size()));
        hash.update(":");
        hash.update(value);
    };
    auto number = [&](std::size_t value) { text(std::to_string(value)); };
    auto span = [&](Span value) { number(value.begin); number(value.end); };
    text("lyra-macro-cache-2");
    text(nativeTriple);
    text(compilerFingerprint());
    text(pathText(code.path));
    text(code.text);
    text(kind);
    text(typeEnvironment);
    text(prefix.name.text);
    span(prefix.name.span);
    number(prefix.decorator);
    number(prefix.argument.has_value());
    if (prefix.argument) { text(prefix.argument->text); span(prefix.argument->span); }
    number(target != nullptr);
    if (target) { text(target->text); span(target->span); }
    number(prefix.exports.size());
    for (const auto& name : prefix.exports) { text(name.text); span(name.span); }
    for (auto* previous = &source; previous; previous = previous->previous.get()) {
        text(pathText(previous->path));
        text(previous->moduleId);
        text(previous->text);
        number(previous->pieces.size());
        for (const auto& piece : previous->pieces) {
            span(piece.output); span(piece.input); number(piece.copied); number(piece.hygiene);
        }
        number(previous->previous != nullptr);
    }
    number(inputs.size());
    for (const auto& [key, input] : inputs) { text(key); text(pathText(input.path)); text(input.text); }
    return llvm::toHex(hash.final(), true);
}

std::optional<std::vector<SyntaxPiece>> readPieces(const json::Value& value, const Source& source) {
    auto* entries = value.getAsArray();
    if (!entries || entries->size() > byteLimit) return {};
    std::vector<SyntaxPiece> pieces;
    std::size_t size = 0;
    for (const auto& entry : *entries) {
        auto* piece = entry.getAsObject();
        if (!piece) return {};
        auto text = piece->getString("text");
        auto copied = piece->getBoolean("copied");
        if (!text || text->empty() || text->size() > byteLimit - size || !copied) return {};
        size += text->size();
        std::optional<Span> span;
        if (auto* range = piece->get("span")) {
            auto* offsets = range->getAsArray();
            if (!offsets || offsets->size() != 2) return {};
            auto begin = (*offsets)[0].getAsUINT64(), end = (*offsets)[1].getAsUINT64();
            if (!begin || !end || *begin > *end || *end > source.text.size()) return {};
            span = Span{static_cast<std::size_t>(*begin), static_cast<std::size_t>(*end)};
        }
        if (*copied && (!span || llvm::StringRef(source.text).slice(span->begin, span->end) != *text)) return {};
        pieces.push_back({text->str(), span, *copied});
    }
    return pieces;
}

std::optional<ExpandedMacro> loadExpansion(const fs::path& path, const Source& source, const std::vector<Field>& fields,
                                           const InputSnapshot& inputs) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return {};
    auto size = fs::file_size(path, error);
    if (error || size <= checksumLength || size > cacheByteLimit) return {};
    std::ifstream file(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!file.read(bytes.data(), bytes.size()) || file.peek() != std::char_traits<char>::eof() || file.bad()) return {};
    auto payload = std::string_view(bytes).substr(checksumLength);
    if (!bytes.starts_with(checksum(pathText(path.stem()), payload))) return {};
    auto value = json::parse(payload);
    if (!value) { llvm::consumeError(value.takeError()); return {}; }
    auto* object = value->getAsObject();
    if (!object) return {};
    auto* bodies = object->getArray("bodies");
    auto* defaults = object->getArray("defaults");
    auto* used = object->getArray("inputs");
    if (!bodies || (bodies->size() != 1 && bodies->size() != 3) || !defaults || !used) return {};
    ExpandedMacro result;
    if (bodies->size() == 3) result.output.hooks.emplace();
    for (const auto& body : *bodies) {
        auto pieces = readPieces(body, source);
        if (!pieces) return {};
        if (result.output.hooks) result.output.hooks->push_back(std::move(*pieces));
        else result.output.code = std::move(*pieces);
    }
    for (const auto& item : *defaults) {
        auto field = item.getAsUINT64();
        if (!field || *field >= fields.size() || !fields[*field].defaultValue || !result.defaultFields.insert(*field).second) return {};
    }
    for (const auto& item : *used) {
        auto key = item.getAsString();
        if (!key || !inputs.contains(key->str())) return {};
        result.inputKeys.push_back(key->str());
    }
    return result;
}

std::optional<json::Array> writePieces(const std::vector<SyntaxPiece>& pieces) {
    json::Array result;
    for (const auto& piece : pieces) {
        if (!json::isUTF8(piece.text)) return {};
        json::Object entry{{"text", piece.text}, {"copied", piece.copied}};
        if (piece.span) entry["span"] = json::Array{piece.span->begin, piece.span->end};
        result.push_back(std::move(entry));
    }
    return result;
}

void storeExpansion(const ExpandedMacro& expansion) {
    const auto& path = expansion.pendingCache;
    if (path.empty()) return;
    json::Array bodies, defaults, inputs;
    if (expansion.output.hooks) {
        for (const auto& hook : *expansion.output.hooks) {
            auto body = writePieces(hook);
            if (!body) return;
            bodies.push_back(std::move(*body));
        }
    } else {
        auto body = writePieces(expansion.output.code);
        if (!body) return;
        bodies.push_back(std::move(*body));
    }
    for (auto field : expansion.defaultFields) defaults.push_back(field);
    for (const auto& key : expansion.inputKeys) inputs.push_back(key);
    auto payload = llvm::formatv("{0}", json::Value(json::Object{{"bodies", std::move(bodies)}, {"defaults", std::move(defaults)}, {"inputs", std::move(inputs)}})).str();
    if (payload.size() > cacheByteLimit - checksumLength) return;
    try {
        StagingDirectory staging(path.parent_path());
        auto temp = staging.path / "expansion";
        std::ofstream file(temp, std::ios::binary);
        file << checksum(pathText(path.stem()), payload) << payload;
        file.close();
        if (!file) return;
        std::error_code ignored;
        fs::rename(temp, path, ignored);
    } catch (const fs::filesystem_error&) { return; }
}

std::string quote(const std::string& text) {
    std::string result = "\"";
    for (char ch : text) {
        switch (ch) {
            case '"': case '\\': case '$': result += '\\'; result += ch; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case '\0': result += "\\0"; break;
            default: result += ch;
        }
    }
    return result + '"';
}

struct Machine {
    const CheckedModule& program;
    const Source& use;
    const Expansion& expansion;
    const std::vector<Field>& fields;
    const std::string& kind;
    const MacroPackage& package;
    std::vector<fs::path>& files;
    const ExpressionExpansion* expression;
    SyntaxArena arena;
    const InputSnapshot* prepared;
    const MacroTypeResolver* resolveType{};
    std::span<const Identifier> parameters{};
    std::span<const Bound> bounds{};
    std::optional<SyntaxRef> target{}, body{};
    unsigned fuel = 50000, depth = 0;
    std::size_t bytes = 0;
    std::map<std::string, std::string> inputs{};
    std::set<std::size_t> defaultFields{};

    [[noreturn]] void error(const std::string& message) const { throw at(use, expansion.name.span, "LYM0002", message); }
    void budget(std::size_t size) {
        bytes += size;
        if (size > byteLimit || bytes > 32 * byteLimit) error("宏展开超出字节预算");
    }
    Value input(const std::string& key) {
        if (auto saved = inputs.find(key); saved != inputs.end()) return saved->second;
        auto grants = package.inputs.find(expansion.name.text);
        if (grants == package.inputs.end() || !grants->second.contains(key)) error("宏未获得只读输入：" + key);
        auto value = prepared ? prepared->at(key) : readInput(use, expansion, package, key, grants->second.at(key));
        files.push_back(std::move(value.path));
        return inputs.emplace(key, std::move(value.text)).first->second;
    }

    Value host(const std::string& name, const std::vector<Value>& args) {
        if (name == "fieldCount") return static_cast<std::int64_t>(fields.size());
        if (name == "targetKind") return kind;
        if (name == "typeParameterCount") return static_cast<std::int64_t>(parameters.size());
        if (name == "typeParameterName") {
            auto index = std::get<std::int64_t>(args.front());
            if (index < 0 || static_cast<std::size_t>(index) >= parameters.size()) error("宏读取的类型形参下标越界");
            return parameters[index].text;
        }
        if (name == "typeParameterDeclarations" || name == "typeParameterArguments") {
            if (parameters.empty()) return std::string{};
            std::string text = "<";
            for (std::size_t index = 0; index < parameters.size(); ++index) {
                if (index) text += ",";
                text += arena.text(parameters[index].span);
                if (name == "typeParameterDeclarations") {
                    bool first = true;
                    for (const auto& bound : bounds) if (bound.parameter == index) {
                        text += std::exchange(first, false) ? ": " : " & ";
                        text += arena.text(bound.trait.span);
                    }
                }
            }
            return text + '>';
        }
        if (name == "syntaxTarget" || name == "syntaxBody") {
            auto ref = name == "syntaxTarget" ? target : body;
            if (!ref) error("此位置没有 " + name + " 所需的语法节点");
            return *ref;
        }
        if (name == "optionInner" || name == "listElement") {
            auto& type = std::get<std::string>(args.front());
            std::string prefix = name == "optionInner" ? "Option<" : "List<";
            return type.starts_with(prefix) && type.ends_with('>') ? type.substr(prefix.size(), type.size() - prefix.size() - 1) : std::string{};
        }
        if (name == "macroFail") error(std::get<std::string>(args.front()));
        if (name == "macroQuote") return quote(std::get<std::string>(args.front()));
        if (name == "macroContains") return std::get<std::string>(args[0]).find(std::get<std::string>(args[1])) != std::string::npos;
        if (name == "macroInput") return input(std::get<std::string>(args.front()));
        if (name == "argumentCount" || name == "syntaxArgument" || name == "argumentString") {
            if (!expression) error("此查询仅适用于表达式宏");
            if (name == "argumentCount") return static_cast<std::int64_t>(expression->arguments.size());
            auto index = std::get<std::int64_t>(args.front());
            if (index < 0 || static_cast<std::size_t>(index) >= expression->arguments.size()) error("表达式宏实参下标越界");
            const auto& arg = expression->arguments[index];
            if (name == "syntaxArgument") {
                auto fragment = sourceText(use.path, arena.text(arg.span), use.moduleId);
                std::vector<SyntaxRef> children;
                for (auto& token : syntaxTokens(fragment)) {
                    auto span = Span{arg.span.begin + token.span.begin, arg.span.begin + token.span.end};
                    children.push_back(arena.input(std::move(token.kind), span));
                }
                return arena.input("argument", arg.span, std::move(children), true);
            }
            if (!arg.literal) throw at(use, arg.span, "LYM0002", "此宏实参需要字符串字面量");
            return *arg.literal;
        }
        if (name.starts_with("syntax")) {
            auto text = [&](std::size_t slot) -> const auto& { return std::get<std::string>(args[slot]); };
            auto syntax = [&](std::size_t slot) { return std::get<SyntaxRef>(args[slot]); };
            auto identifier = [&](std::size_t slot) {
                const auto& value = text(slot);
                auto letters = [](unsigned char ch) { return ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); };
                if (value.empty() || !letters(value.front()) || !std::ranges::all_of(value, [&](unsigned char ch) { return letters(ch) || (ch >= '0' && ch <= '9'); }))
                    error("不是有效的 Lyra 名称：" + value);
                return '`' + value + '`';
            };
            if (name == "syntaxCode") return arena.add({SyntaxForm::Code, "code", text(0)});
            if (name == "syntaxCodeAt") return arena.add({SyntaxForm::Code, "code", text(1), {}, arena.get(syntax(0)).span});
            if (name == "syntaxString") return arena.add({SyntaxForm::Code, "string", quote(text(0))});
            if (name == "syntaxInt") return arena.add({SyntaxForm::Code, "number", std::to_string(std::get<std::int64_t>(args[0]))});
            if (name == "syntaxName") return arena.add({SyntaxForm::Code, "name", identifier(0)});
            if (name == "syntaxJoin" || name == "syntaxArgs" || name == "syntaxConcat") return arena.add({SyntaxForm::Join, "sequence", name == "syntaxJoin" ? "\n" : name == "syntaxArgs" ? ", " : "", {syntax(0), syntax(1)}});
            if (name == "syntaxCall") return arena.add({SyntaxForm::Call, "call", {}, {syntax(0), syntax(1)}});
            if (name == "syntaxMember") return arena.add({SyntaxForm::Member, "member", identifier(1), {syntax(0)}});
            if (name == "syntaxBlock") return arena.add({SyntaxForm::Block, "block", {}, {syntax(0)}});
            if (name == "syntaxAround") return arena.add({SyntaxForm::Around, "around", {}, {syntax(0), syntax(1), syntax(2)}});
            auto ref = syntax(0);
            auto& node = arena.get(ref);
            if (name == "syntaxResolvedType") {
                if (!resolveType || node.canonical.empty()) error("已解析类型查询只适用于目标声明的显式类型标注");
                auto span = *node.span;
                auto append = [&](auto&& self, const MacroType& type) -> SyntaxRef {
                    std::vector<SyntaxRef> arguments;
                    for (const auto& part : type.arguments) arguments.push_back(self(self, part));
                    return arena.add({SyntaxForm::Type, type.kind, type.name, std::move(arguments), span, {}, type.module});
                };
                return append(append, (*resolveType)(expansion.file, Identifier{node.canonical, span}, parameters));
            }
            if (name == "syntaxTypeModule") {
                if (node.form != SyntaxForm::Type) error("syntaxTypeModule 需要已解析类型节点");
                return node.module;
            }
            if (name == "syntaxKind") return node.kind;
            if (name == "syntaxIdentifier") {
                if (node.kind != "name" || node.form != SyntaxForm::Input) error("syntaxIdentifier 需要源码名称节点");
                auto value = arena.text(*node.span);
                return value.starts_with('`') ? value.substr(1, value.size() - 2) : value;
            }
            if (name == "syntaxChildCount") return static_cast<std::int64_t>(node.children.size());
            if (name == "syntaxChild") {
                auto index = std::get<std::int64_t>(args[1]);
                if (index < 0 || static_cast<std::size_t>(index) >= node.children.size()) error("语法子节点下标越界");
                return node.children[index];
            }
            if (name == "syntaxText") {
                if (node.form == SyntaxForm::Type) return node.text;
                if (node.span) return arena.text(*node.span);
                std::string generated;
                for (auto& piece : arena.render(ref)) generated += piece.text;
                return generated;
            }
            auto range = node.span.value_or(expansion.name.span);
            if (name == "syntaxError") throw at(use, range, "LYM0002", text(1));
            auto site = use.userSite(range);
            auto value = name == "syntaxStart" ? site.start.offset : name == "syntaxEnd" ? site.end.offset : name == "syntaxLine" ? site.start.line : site.start.column;
            return static_cast<std::int64_t>(value);
        }
        auto index = std::get<std::int64_t>(args.front());
        if (index < 0 || static_cast<std::size_t>(index) >= fields.size()) error("宏读取的字段或形参下标越界");
        const auto& field = fields[index];
        if (name == "fieldName") return field.name.text;
        if (name == "fieldType") return field.type.text;
        if (name == "fieldDefaultKind" || name == "fieldDefaultFunction") {
            if (kind != "struct") error("字段默认值查询需要 struct 声明宏");
            if (name == "fieldDefaultKind") return std::string(!field.defaultValue ? "required" : field.defaultValue->emptyOption ? "none" : "expression");
            if (!field.defaultValue) error("字段没有默认值：" + field.name.text);
            defaultFields.insert(index);
            return fieldDefaultReference(index);
        }
        if (name == "fieldHasAttribute" || name == "fieldAttributeValue") {
            auto found = std::ranges::find_if(field.attributes, [&](const auto& mark) { return mark.name.text == std::get<std::string>(args[1]); });
            if (name == "fieldHasAttribute") return found != field.attributes.end();
            if (found == field.attributes.end()) error("字段或形参缺少标记：" + std::get<std::string>(args[1]));
            return found->argument ? found->argument->text : std::string{};
        }
        return field.isPublic;
    }

    Value invoke(const CheckedFunction& fn, const std::vector<Value>& args) {
        if (fn.syntax.file == 0) return host(fn.syntax.name.text, args);
        if (++depth > 64) error("宏调用深度超过 64");
        std::vector<Value> locals(fn.locals.size());
        std::copy(args.begin(), args.end(), locals.begin());
        Value result;
        try { result = eval(fn, fn.syntax.body, locals); } catch (Returned& ret) { result = std::move(ret.value); }
        --depth;
        return result;
    }

    Value math(Op op, const Value& left, const Value& right) {
        if (op == Op::Eq || op == Op::Ne) return op == Op::Eq ? left == right : left != right;
        if (op == Op::Add && std::holds_alternative<std::string>(left)) return std::get<std::string>(left) + std::get<std::string>(right);
        auto first = std::get<std::int64_t>(left), second = std::get<std::int64_t>(right);
        std::int64_t result;
        switch (op) {
            case Op::Add: if (!__builtin_add_overflow(first, second, &result)) return result; break;
            case Op::Sub: if (!__builtin_sub_overflow(first, second, &result)) return result; break;
            case Op::Mul: if (!__builtin_mul_overflow(first, second, &result)) return result; break;
            case Op::Div: if (second && !(first == std::numeric_limits<std::int64_t>::min() && second == -1)) return first / second; break;
            case Op::Rem: if (second == -1) return std::int64_t{0}; if (second) return first % second; break;
            case Op::Lt: return first < second;
            case Op::Le: return first <= second;
            case Op::Gt: return first > second;
            case Op::Ge: return first >= second;
            default: error("此运算不能用于编译期宏");
        }
        error("宏中整数溢出或除零");
    }

    std::string display(Value value) {
        if (auto integer = std::get_if<std::int64_t>(&value)) return std::to_string(*integer);
        if (auto boolean = std::get_if<bool>(&value)) return *boolean ? "true" : "false";
        return std::get<std::string>(std::move(value));
    }

    Value eval(const CheckedFunction& fn, ExprId id, std::vector<Value>& locals) {
        if (!fuel--) error("宏展开超过执行步数预算");
        auto sub = [&](ExprId child) { return eval(fn, child, locals); };
        const auto& info = fn.expressions[id];
        const auto& node = fn.syntax.expressions[id].node;
        Value result;
        if (auto value = std::get_if<IntegerLiteral>(&node)) result = static_cast<std::int64_t>(integerBits(*value));
        else if (auto value = std::get_if<bool>(&node)) result = *value;
        else if (auto str = std::get_if<StringLiteral>(&node)) result = str->bytes;
        else if (auto item = std::get_if<DisplayValue>(&node)) result = display(sub(item->value));
        else if (auto text = std::get_if<TextTemplate>(&node)) {
            std::string combined;
            for (auto part : text->parts) {
                combined += std::get<std::string>(sub(part));
                budget(combined.size());
            }
            result = std::move(combined);
        }
        else if (std::holds_alternative<Identifier>(node)) result = locals[info.target];
        else if (auto binding = std::get_if<Binding>(&node)) locals[info.target] = sub(binding->value);
        else if (auto block = std::get_if<Block>(&node)) {
            for (auto& statement : block->statements) { result = sub(statement.expression); if (statement.semicolon) result = {}; }
        } else if (auto branch = std::get_if<Conditional>(&node)) {
            if (std::get<bool>(sub(branch->condition))) result = sub(branch->yes);
            else if (branch->no) result = sub(*branch->no);
        } else if (auto loop = std::get_if<While>(&node)) {
            while (std::get<bool>(sub(loop->condition))) {
                try { sub(loop->body); } catch (Jump jump) { if (jump == Jump::Break) break; }
            }
        } else if (auto jump = std::get_if<Jump>(&node)) throw *jump;
        else if (auto ret = std::get_if<Return>(&node)) throw Returned{ret->value ? sub(*ret->value) : Value{}};
        else if (auto unary = std::get_if<Unary>(&node)) {
            auto operand = sub(unary->operand);
            result = unary->op == Op::Not ? Value(!std::get<bool>(operand)) : math(Op::Sub, std::int64_t{0}, operand);
        } else if (auto binary = std::get_if<Binary>(&node)) {
            auto op = binary->op;
            if (op <= Op::RemSet) {
                if (!std::holds_alternative<Identifier>(fn.syntax.expressions[binary->left].node)) error("宏中只能写入局部绑定");
                auto slot = fn.expressions[binary->left].target;
                auto left = locals[slot];
                auto right = sub(binary->right);
                constexpr Op operations[] = {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Rem};
                locals[slot] = op == Op::Set ? right : math(operations[static_cast<int>(op) - 1], left, right);
                if (auto str = std::get_if<std::string>(&locals[slot])) budget(str->size());
            } else {
                auto left = sub(binary->left);
                if (op == Op::And) result = std::get<bool>(left) && std::get<bool>(sub(binary->right));
                else if (op == Op::Or) result = std::get<bool>(left) || std::get<bool>(sub(binary->right));
                else result = math(op, left, sub(binary->right));
            }
        } else if (auto call = std::get_if<Call>(&node)) {
            if (info.callee == Callee::ToString) result = display(sub(*call->receiver));
            else {
                if (info.callee != Callee::Function || call->receiver) error("编译期不能调用运行时函数：" + call->name.text);
                const auto& callee = program.functions[info.target];
                if (call->arguments.size() != callee.syntax.parameters.size()) error("编译期宏调用需要显式传入每个形参");
                std::vector<Value> args(call->arguments.size());
                for (std::size_t index = 0; index < args.size(); ++index) args[info.argumentSlots[index]] = sub(call->arguments[index].value);
                result = invoke(callee, args);
            }
        } else error("此表达式不属于受限的编译期 Lyra 子集");
        if (auto str = std::get_if<std::string>(&result)) budget(str->size());
        return result;
    }
};

struct MacroDeclaration {
    const Function* function{};
    const Aggregate* aggregate{};
    const Enumeration* enumeration{};
    const MacroTypeResolver* resolveType{};
    std::string_view typeEnvironment{};
};

ExpandedMacro expand(const Source& source, const Expansion& prefix, const MacroPackage& package, const Identifier* target, const std::vector<Field>& fields, const std::string& kind, std::vector<fs::path>& files, const ExpressionExpansion* expression = nullptr, MacroDeclaration declaration = {}) {
    auto directory = expression ? "expressions" : prefix.decorator ? "decorators" : "macros";
    auto file = prefix.name.text + ".ly";
    auto path = package.root / "src" / (std::string(".") + directory) / file;
    auto overlaid = package.overlays && package.overlays->contains(path);
    if (overlaid || fs::is_regular_file(path)) {
        auto relative = fs::weakly_canonical(path).lexically_relative(fs::canonical(package.root));
        if (relative.empty() || *relative.begin() == "..") throw at(source, prefix.name.span, "LYM0002", "宏实现必须位于声明包内");
    } else {
        path = sdkRoot() / "share/lyra/std/meta" / directory / file;
    }
    if (!overlaid && !fs::is_regular_file(path)) throw at(source, prefix.name.span, "LYM0001", "找不到" + std::string(expression ? "表达式宏 #" : prefix.decorator ? "装饰器 @" : "声明宏 #") + prefix.name.text);
    if ((overlaid ? package.overlays->at(path).size() : fs::file_size(path)) > byteLimit) throw at(source, prefix.name.span, "LYM0002", "宏实现超过大小限制");
    files.push_back(path);
    auto implementation = loadSource(path, {}, overlaid ? package.overlays : nullptr);
    std::optional<InputSnapshot> prepared;
    fs::path cache;
    if (!package.cache.empty() && !compilerFingerprint().empty()) prepared = snapshotInputs(source, prefix, package);
    if (prepared) {
        cache = package.cache / fromUtf8(cacheKey(source, prefix, implementation, target, kind, *prepared, declaration.typeEnvironment) + ".expansion");
        if (auto hit = loadExpansion(cache, source, fields, *prepared)) {
            for (const auto& key : hit->inputKeys) files.push_back(prepared->at(key).path);
            return std::move(*hit);
        }
    }
    std::vector<Source> code{sourceText("<macro-prelude>", prelude, {}), std::move(implementation)};
    Module parsed;
    parseFile(parsed, code[0], 0);
    parseFile(parsed, code[1], 1);
    if (!parsed.imports.empty() || parsed.aggregates.size() != 1 || !parsed.expansions.empty() || !parsed.traits.empty() || !parsed.enums.empty() || !parsed.extensions.empty() || !parsed.exceptions.empty()) throw at(source, prefix.name.span, "LYM0002", "宏实现只能声明同步的纯计算函数");
    for (const auto& fn : parsed.functions) if (fn.isAsync || fn.foreignSymbol || fn.conversion || !fn.typeParameters.empty() || fn.constant != ConstKind::None) throw at(source, prefix.name.span, "LYM0002", "宏不能使用 extern、异步、泛型入口、隐式转换或 const 声明");
    auto checked = check(std::move(parsed), code, false);
    const auto syntaxType = Type{Type::Record, 0};
    auto scalar = [&](Type type) { return type == Type::Int || type == Type::Bool || type == Type::String || type == Type::Unit || type == Type::Never || type == syntaxType; };
    for (const auto& fn : checked.functions) {
        if (!scalar(fn.result) || !std::ranges::all_of(fn.locals, [&](const auto& local) { return scalar(local.type); }) ||
            !std::ranges::all_of(fn.expressions, [&](const auto& expr) { return scalar(expr.type) && (!expr.sourceType || scalar(*expr.sourceType)); })) {
            throw at(source, prefix.name.span, "LYM0002", "编译期宏只可计算 Int、Bool、String、Unit 和 Syntax");
        }
    }
    auto it = std::find_if(checked.functions.begin(), checked.functions.end(), [&](const auto& fn) { return fn.syntax.file == 1 && fn.syntax.name.text == prefix.name.text; });
    if (it == checked.functions.end() || it->result != syntaxType || it->syntax.parameters.size() != 2 || it->locals[0].type != Type::String || it->locals[1].type != Type::String) {
        throw at(source, prefix.name.span, "LYM0002", "宏入口应为 fn " + prefix.name.text + "(target: String, argument: String): Syntax");
    }
    auto [function, aggregate, enumeration, resolveType, environment] = declaration;
    Machine machine{checked, source, prefix, fields, kind, package, files, expression, {source, prefix.name.span}, prepared ? &*prepared : nullptr, resolveType};
    auto generics = [&](const auto& item) { machine.parameters = item.typeParameters; machine.bounds = item.bounds; };
    if (function) generics(*function);
    if (aggregate) generics(*aggregate);
    if (enumeration) generics(*enumeration);
    if (function && !function->foreignSymbol) machine.body = machine.arena.expression(*function, function->body);
    if (target) machine.target = machine.arena.declaration(*target, fields, function, aggregate, enumeration, machine.body);
    auto output = std::get<SyntaxRef>(machine.invoke(*it, {target ? target->text : std::string{}, prefix.argument ? prefix.argument->text : std::string{}}));
    ExpandedMacro result{{}, std::move(machine.defaultFields), {}, std::move(cache)};
    for (const auto& [key, text] : machine.inputs) result.inputKeys.push_back(key);
    if (machine.arena.get(output).form != SyntaxForm::Around) result.output.code = machine.arena.render(output);
    else {
        if (!prefix.decorator) machine.error("syntaxAround 只能在装饰器中使用");
        result.output.hooks.emplace();
        for (auto step : machine.arena.get(output).children) result.output.hooks->push_back(machine.arena.render(step));
    }
    return result;
}

Source generatedSource(const Source& source, const Expansion& prefix, const std::vector<SyntaxPiece>& code) {
    std::string text;
    std::vector<SourcePiece> pieces;
    auto site = prefix.name.span;
    auto mark = source.userSite(site).start.offset + 1;
    for (const auto& piece : code) {
        if (piece.text.empty()) continue;
        pieces.push_back({{text.size(), text.size() + piece.text.size()}, piece.span.value_or(site), piece.copied, mark});
        text += piece.text;
    }
    if (pieces.empty()) pieces.push_back({{0, 0}, site, false, mark});
    auto suffix = (prefix.decorator ? "@" : "#") + prefix.name.text;
    auto result = sourceText(fromUtf8(pathText(source.path) + "(" + suffix + ")"), std::move(text), source.moduleId);
    result.origin = source.userSite(site);
    result.previous = std::make_shared<Source>(source);
    result.pieces = std::move(pieces);
    return result;
}
}

void parsePackageFile(Module& syntax, Source& source, std::size_t file, const MacroPackage& package, std::vector<fs::path>& inputs) {
    const auto limit = source.text.size() + 4 * byteLimit;
    for (unsigned depth = 0; source.text.find('#') != std::string::npos; ++depth) {
        Module scratch;
        std::vector<ExpressionExpansion> calls;
        parseFile(scratch, source, file, &calls);
        if (calls.empty()) break;
        if (depth == 32) throw at(source, calls.front().name.span, "LYM0002", "表达式宏嵌套超过 32 层");
        std::string text;
        std::vector<SourcePiece> pieces;
        std::size_t read = 0;
        const auto original = std::string_view(source.text);
        for (const auto& call : calls) {
            auto site = call.name.span;
            auto append = [&](std::string_view bytes, Span input, bool copied, std::size_t hygiene) {
                if (bytes.empty()) return;
                if (bytes.size() > limit - text.size()) throw at(source, site, "LYM0002", "展开后源码超过字节预算");
                pieces.push_back({{text.size(), text.size() + bytes.size()}, input, copied, hygiene});
                text += bytes;
            };
            auto copy = [&](Span span) { append(original.substr(span.begin, span.end - span.begin), span, true, 0); };
            copy({read, call.span.begin});
            auto argument = call.argumentsSpan;
            auto wrapper = call.arguments.empty() ? call.name.span : call.argumentsSpan;
            Expansion prefix{file, call.name, Identifier{std::string(original.substr(argument.begin, argument.end - argument.begin)), argument}, false, ExpansionTarget::Function, 0};
            auto generated = expand(source, prefix, package, nullptr, {}, "expression", inputs, &call);
            std::string skeleton;
            for (const auto& piece : generated.output.code) skeleton += piece.copied && piece.text.contains('#') ? "0" : piece.text;
            auto output = sourceText(fromUtf8(pathText(source.path) + "(#" + call.name.text + ")"), std::move(skeleton), source.moduleId);
            output.origin = source.site(site);
            validateMacroExpression(output);
            auto mark = output.origin->start.offset + 1;
            append("(", wrapper, false, mark);
            for (const auto& piece : generated.output.code) append(piece.text, piece.span.value_or(site), piece.copied, mark);
            append("\n)", wrapper, false, mark);
            read = call.span.end;
            storeExpansion(generated);
        }
        if (limit - text.size() < original.size() - read) throw at(source, calls.back().name.span, "LYM0002", "展开后源码超过字节预算");
        if (read < original.size()) {
            pieces.push_back({{text.size(), text.size() + original.size() - read}, {read, original.size()}, true, 0});
            text += original.substr(read);
        }
        auto next = sourceText(source.path, std::move(text), source.moduleId);
        next.previous = std::make_shared<Source>(std::move(source));
        next.pieces = std::move(pieces);
        source = std::move(next);
    }
    parseFile(syntax, source, file);
}

void expandMacros(Module& syntax, std::vector<Source>& sources, const std::map<std::string, MacroPackage>& packages,
    std::vector<fs::path>& inputs, const MacroTypeResolver& resolveType, std::string_view typeEnvironment) {
    auto prefixes = std::exchange(syntax.expansions, {});
    std::vector<std::pair<Expansion, ExpandedMacro>> decorators;
    for (const auto& prefix : prefixes) {
        const auto& source = sources[prefix.file];
        if (prefix.decorator && prefix.name.text == "derive") throw at(source, prefix.name.span, "LYM0001", "derive 是声明宏；请写 #derive(Json)");
        std::vector<Field> fields;
        MacroDeclaration declaration{.resolveType = &resolveType, .typeEnvironment = typeEnvironment};
        if (prefix.kind == ExpansionTarget::Aggregate) {
            declaration.aggregate = &syntax.aggregates[prefix.target];
            fields = declaration.aggregate->fields;
        } else if (prefix.kind == ExpansionTarget::Enumeration) {
            declaration.enumeration = &syntax.enums[prefix.target];
        } else {
            declaration.function = &syntax.functions[prefix.target];
            if (prefix.decorator && (declaration.function->foreignSymbol || declaration.function->conversion)) throw at(source, prefix.name.span, "LYM0002", "装饰器需要普通函数体");
            for (const auto& param : declaration.function->parameters) fields.push_back({param.name, param.type, false, true, param.attributes});
        }
        const auto& name = declaration.enumeration ? declaration.enumeration->name : declaration.aggregate ? declaration.aggregate->name : declaration.function->name;
        auto kind = declaration.enumeration ? "enum" : declaration.aggregate ? declaration.aggregate->reference ? "class" : "struct" : "fn";
        auto generated = expand(source, prefix, packages.at(source.moduleId), &name, fields, kind, inputs, nullptr, declaration);
        if (prefix.decorator) {
            if (prefix.kind == ExpansionTarget::Aggregate) {
                if (generated.output.hooks) throw at(source, prefix.name.span, "LYM0002", "类型装饰器应生成辅助声明，不能返回 syntaxAround");
            } else {
                if (!prefix.exports.empty()) throw at(source, prefix.name.span, "LYM0002", "函数装饰器不能生成公开声明");
                if (!generated.output.hooks) throw at(source, prefix.name.span, "LYM0002", "函数装饰器应返回 syntaxAround(before, after, failure)");
                decorators.emplace_back(prefix, std::move(generated));
                continue;
            }
        }
        auto output = generatedSource(source, prefix, generated.output.code);
        sources.push_back(std::move(output));
        auto start = syntax.functions.size(), records = syntax.aggregates.size(), imports = syntax.imports.size(), traits = syntax.traits.size(), enums = syntax.enums.size(), extensions = syntax.extensions.size(), exceptions = syntax.exceptions.size(), aliases = syntax.aliases.size();
        parseFile(syntax, sources.back(), sources.size() - 1);
        std::map<std::string, std::size_t> defaultReferences;
        for (auto field : generated.defaultFields) defaultReferences.emplace(fieldDefaultReference(field), fields[field].defaultValue->function);
        if (!defaultReferences.empty()) for (auto index = start; index < syntax.functions.size(); ++index) {
            for (auto& expr : syntax.functions[index].expressions) {
                auto* reference = std::get_if<Identifier>(&expr.node);
                if (auto* call = std::get_if<Call>(&expr.node); call && !call->receiver && !call->function && !call->typeOwner) reference = &call->name;
                if (!reference || sources.back().text[reference->span.begin] == '`') continue;
                if (auto found = defaultReferences.find(reference->text); found != defaultReferences.end()) reference->text = syntax.functions[found->second].name.text;
            }
        }
        const auto& original = sources[prefix.file];
        auto reject = [&] { throw at(original, prefix.name.span, "LYM0002", "宏不能自行生成公开声明、extern 或新的宏调用"); };
        std::map<std::string, bool> allowed;
        for (const auto& request : prefix.exports) allowed.emplace(request.text, false);
        auto publish = [&](const std::string& name, bool topLevel) {
            auto candidate = allowed.find(name);
            if (candidate == allowed.end() || name == "main") reject();
            if (topLevel) candidate->second = true;
        };
        for (auto index = start; index < syntax.functions.size(); ++index) {
            const auto& fn = syntax.functions[index];
            if (fn.foreignSymbol || (!fn.owner && !fn.extensionGroup && fn.name.text == "main")) reject();
            if (fn.isPublic) publish(fn.owner ? fn.owner->text : fn.extensionGroup ? fn.extensionGroup->text : fn.name.text, !fn.owner && !fn.extensionGroup);
        }
        auto publishAll = [&](const auto& declarations, std::size_t from) {
            for (auto index = from; index < declarations.size(); ++index)
                if (declarations[index].isPublic) publish(declarations[index].name.text, true);
        };
        publishAll(syntax.traits, traits); publishAll(syntax.aggregates, records); publishAll(syntax.enums, enums);
        publishAll(syntax.extensions, extensions); publishAll(syntax.exceptions, exceptions); publishAll(syntax.aliases, aliases);
        if (!syntax.expansions.empty() || std::any_of(syntax.imports.begin() + imports, syntax.imports.end(), [](const auto& use) { return use.isPublic; })) reject();
        for (const auto& request : prefix.exports) if (!allowed.at(request.text))
            throw at(original, request.span, "LYM0002", "宏没有生成所请求的公开声明：" + request.text);
        storeExpansion(generated);
    }
    for (auto item = decorators.rbegin(); item != decorators.rend(); ++item) {
        auto& [prefix, generated] = *item;
        auto site = prefix.name.span;
        auto start = std::to_string(syntax.functions.size());
        bool enabled[] = {false, false, false};
        const std::string parameters[] = {"", "result: Any", "exception: Exception"};
        std::string hooks[3];
        for (std::size_t slot = 0; slot < 3; ++slot) {
            const auto& pieces = (*generated.output.hooks)[slot];
            if (pieces.empty()) continue;
            auto body = pieces;
            body.insert(body.begin(), {"fn hook(" + parameters[slot] + "): Unit {\n"});
            body.push_back({"\n{}\n}"});
            auto source = generatedSource(sources[prefix.file], prefix, body);
            Module parsed;
            parseFile(parsed, source, sources.size());
            if (parsed.functions.size() != 1 || !parsed.aggregates.empty() || !parsed.imports.empty() || !parsed.expansions.empty() ||
                !parsed.traits.empty() || !parsed.enums.empty() || !parsed.extensions.empty() || !parsed.exceptions.empty() || !parsed.aliases.empty())
                throw at(sources[prefix.file], site, "LYM0002", "装饰器阶段只能生成块中的语句");
            auto& hook = parsed.functions.front();
            enabled[slot] = true;
            hooks[slot] = "$decoratorHook" + start + "_" + std::to_string(slot);
            hook.name.text = hooks[slot];
            sources.push_back(std::move(source));
            syntax.functions.push_back(std::move(hook));
        }
        auto hidden = syntax.functions[prefix.target];
        hidden.name.text = "$decorated" + start;
        hidden.isPublic = false;
        for (auto& param : hidden.parameters) param.defaultValue.reset();
        auto name = hidden.name.text;
        syntax.functions.push_back(std::move(hidden));
        auto& fn = syntax.functions[prefix.target];
        auto add = [&](auto node) {
            fn.expressions.push_back({site, std::move(node)});
            return fn.expressions.size() - 1;
        };
        auto block = [&](std::vector<Statement> statements) { return add(Block{std::move(statements), site}); };
        auto call = [&](const std::string& name, std::vector<Argument> args) { return add(Call{{name, site}, std::move(args)}); };
        std::vector<Argument> args;
        for (const auto& param : fn.parameters) args.push_back({{}, add(Identifier{param.name.text, param.name.span}), param.variadic ? std::optional(site) : std::nullopt});
        auto value = add(Call{{name, site}, std::move(args), {}, fn.typeParameters});
        if (fn.isAsync) value = add(TaskOp{false, site, value});
        if (enabled[2]) {
            auto exception = Identifier{"$decoratorException", site};
            auto failure = call(hooks[2], {{{}, add(exception)}});
            auto rethrow = add(Raise{site, std::nullopt});
            auto handler = block({{failure, site}, {rethrow, {}}});
            value = add(Attempt{block({{value, {}}}), {{site, CatchBinding{exception, {"Exception", site}}, {}, handler}}});
        }
        std::vector<Statement> statements;
        if (enabled[0]) statements.push_back({call(hooks[0], {}), site});
        if (enabled[1]) {
            auto result = Identifier{"$decoratorResult", site};
            statements.push_back({add(Binding{result, false, {}, value}), site});
            statements.push_back({call(hooks[1], {{{}, add(result)}}), site});
            value = add(result);
        }
        statements.push_back({value, {}});
        fn.body = block(std::move(statements));
        storeExpansion(generated);
    }
}
}
