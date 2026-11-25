module;
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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
import lyra.platform;

export namespace lyra {
inline constexpr std::size_t macroInputByteLimit = 1024 * 1024;
using MacroFiles = std::map<std::string, std::map<std::string, std::filesystem::path>>;
struct MacroPackage { std::filesystem::path root; MacroFiles inputs; const SourceOverlay* overlays{}; std::filesystem::path cache{}; };
enum class MacroKind { Declaration, Expression, Decorator };
struct MacroOrigin { std::filesystem::path path; std::string package, name; bool imported; };
using MacroResolver = std::function<MacroOrigin(const Source&, const Identifier&, MacroKind)>;
using MacroLinker = std::function<std::string(const MacroOrigin&, Module&, std::vector<Source>&)>;
struct MacroEnvironment { const MacroResolver& resolve; const MacroLinker& link; };
struct MacroType { std::string kind, name, module; std::vector<MacroType> arguments{}; };
using MacroTypeResolver = std::function<MacroType(std::size_t, const Identifier&, std::span<const Identifier>)>;
void expandMacros(Module& syntax, std::vector<Source>& sources, const std::map<std::string, MacroPackage>& packages,
    std::vector<std::filesystem::path>& inputs, const MacroTypeResolver& resolveType, std::string_view typeEnvironment, const MacroEnvironment& environment);
void expandExpressions(Source& source, std::size_t file, const MacroPackage& package, std::vector<std::filesystem::path>& inputs,
    const MacroEnvironment& environment);
std::string_view macroDirectory(MacroKind kind);
}

namespace lyra {
namespace fs = std::filesystem;

std::string_view macroDirectory(MacroKind kind) {
    switch (kind) {
        case MacroKind::Declaration: return "macros";
        case MacroKind::Expression: return "expressions";
        case MacroKind::Decorator: return "decorators";
    }
    std::abort();
}

namespace {
namespace json = llvm::json;
constexpr auto byteLimit = macroInputByteLimit;
constexpr std::size_t cacheByteLimit = 16 * byteLimit;
constexpr std::size_t checksumLength = 64;
constexpr auto prelude = R"(pub struct Syntax(val handle: Int)
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
fn quoteText(text: String): Syntax { Syntax(0) }
fn quoteJoin(left: Syntax, right: Syntax): Syntax { left }
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
                     std::string_view kind, const InputSnapshot& inputs, std::string_view typeEnvironment, std::string_view dependencies) {
    llvm::SHA256 hash;
    auto text = [&](std::string_view value) {
        hash.update(std::to_string(value.size()));
        hash.update(":");
        hash.update(value);
    };
    auto number = [&](std::size_t value) { text(std::to_string(value)); };
    auto span = [&](Span value) { number(value.begin); number(value.end); };
    text("lyra-macro-cache-3");
    text(nativeTriple);
    text(compilerFingerprint());
    text(pathText(code.path));
    text(code.text);
    text(kind);
    text(typeEnvironment);
    text(dependencies);
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

struct MacroContext {
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
    std::map<std::string, std::string> inputs{};
    std::set<std::size_t> defaultFields{};

    [[noreturn]] void error(const std::string& message) const { throw at(use, expansion.name.span, "LYM0002", message); }
    MacroValue input(const std::string& key) {
        if (auto saved = inputs.find(key); saved != inputs.end()) return saved->second;
        auto grants = package.inputs.find(expansion.name.text);
        if (grants == package.inputs.end() || !grants->second.contains(key)) error("宏未获得只读输入：" + key);
        auto value = prepared ? prepared->at(key) : readInput(use, expansion, package, key, grants->second.at(key));
        files.push_back(std::move(value.path));
        return inputs.emplace(key, std::move(value.text)).first->second;
    }

    MacroValue host(std::string_view name, std::span<const MacroValue> args) {
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
            if (!ref) error("此位置没有 " + std::string(name) + " 所需的语法节点");
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
        if (name == "$quoteText") return arena.add({SyntaxForm::Code, "quote", std::get<std::string>(args[0])});
        if (name == "$quoteJoin") return arena.add({SyntaxForm::Join, "quote", {}, {std::get<SyntaxRef>(args[0]), std::get<SyntaxRef>(args[1])}});
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

};

struct MacroDeclaration {
    const Function* function{};
    const Aggregate* aggregate{};
    const Enumeration* enumeration{};
    const MacroTypeResolver* resolveType{};
    std::string_view typeEnvironment{};
};

ExpandedMacro expand(const Source& source, const Expansion& prefix, const MacroPackage& package, const Identifier* target,
    const std::vector<Field>& fields, const std::string& kind, std::vector<fs::path>& files, const MacroEnvironment& environment,
    const ExpressionExpansion* expression = nullptr, MacroDeclaration declaration = {}) {
    auto origin = environment.resolve(source, prefix.name, expression ? MacroKind::Expression : prefix.decorator ? MacroKind::Decorator : MacroKind::Declaration);
    const auto& path = origin.path;
    auto overlaid = package.overlays && package.overlays->contains(path);
    if ((overlaid ? package.overlays->at(path).size() : fs::file_size(path)) > byteLimit) throw at(source, prefix.name.span, "LYM0002", "宏实现超过大小限制");
    files.push_back(path);
    std::vector<Source> code{sourceText("<macro-prelude>", prelude, "$macroPrelude"), loadSource(path, "$macroImplementation", overlaid ? package.overlays : nullptr)};
    Module parsed;
    parseFile(parsed, code[0], 0);
    for (auto& fn : parsed.functions) {
        fn.isPublic = true;
        if (fn.name.text == "quoteText" || fn.name.text == "quoteJoin") fn.name.text.insert(0, "$");
    }
    auto start = parsed.functions.size();
    std::vector<ExpressionExpansion> calls;
    parseFile(parsed, code[1], 1, &calls, true);
    if (!calls.empty() || !parsed.expansions.empty() || std::ranges::any_of(parsed.imports, [](const auto& use) { return use.kind != ImportKind::Runtime; }))
        throw at(source, prefix.name.span, "LYM0004", "编译期阶段环：宏实现不能调用其他宏或装饰器");
    std::optional<std::size_t> entry;
    for (auto index = start; index < parsed.functions.size(); ++index) {
        const auto& fn = parsed.functions[index];
        if (fn.isAsync || fn.foreignSymbol || fn.conversion)
            throw at(source, prefix.name.span, "LYM0002", "宏实现不能声明异步、extern 或 implicit 函数");
        if (fn.owner || fn.extensionGroup || fn.name.text != origin.name) continue;
        if (entry || fn.constant == ConstKind::Value || !fn.typeParameters.empty() ||
            fn.result.text.empty() || std::ranges::any_of(fn.parameters, [](const auto& param) { return param.variadic || param.defaultValue; }))
            throw at(source, prefix.name.span, "LYM0002", "宏入口必须是无重载、泛型、默认实参或变长形参的同步函数");
        entry = index;
    }
    if (!entry) throw at(source, prefix.name.span, "LYM0002", "宏实现缺少入口 fn " + origin.name + "(target: String, argument: String): Syntax");
    if (origin.imported && !parsed.functions[*entry].isPublic)
        throw at(source, prefix.name.span, "LYM0001", "导入的宏或装饰器入口必须是 pub fn：" + origin.name);
    auto dependencies = environment.link(origin, parsed, code);
    std::optional<InputSnapshot> prepared;
    fs::path cache;
    if (!package.cache.empty() && !compilerFingerprint().empty()) prepared = snapshotInputs(source, prefix, package);
    if (prepared) {
        cache = package.cache / fromUtf8(cacheKey(source, prefix, code[1], target, kind, *prepared, declaration.typeEnvironment, dependencies) + ".expansion");
        if (auto hit = loadExpansion(cache, source, fields, *prepared)) {
            for (const auto& key : hit->inputKeys) files.push_back(prepared->at(key).path);
            return std::move(*hit);
        }
    }
    auto [function, aggregate, enumeration, resolveType, typeEnvironment] = declaration;
    MacroContext machine{source, prefix, fields, kind, package, files, expression, {source, prefix.name.span}, prepared ? &*prepared : nullptr, resolveType};
    auto generics = [&](const auto& item) { machine.parameters = item.typeParameters; machine.bounds = item.bounds; };
    if (function) generics(*function);
    if (aggregate) generics(*aggregate);
    if (enumeration) generics(*enumeration);
    if (function && !function->foreignSymbol) machine.body = machine.arena.expression(*function, function->body);
    if (target) machine.target = machine.arena.declaration(*target, fields, function, aggregate, enumeration, machine.body);
    MacroExecution execution{*entry, 0, target ? target->text : std::string{}, prefix.argument ? prefix.argument->text : std::string{},
        [&](auto name, auto arguments) { return machine.host(name, arguments); }};
    try { check(std::move(parsed), code, false, &execution); }
    catch (const Diagnostic& failure) {
        if (failure.code == "LYM0002" && failure.site && failure.site->file == source.userSite(prefix.name.span).file) throw;
        auto phase = failure.code == "LYM0004" || failure.code == "LYN0001" || failure.code == "LYT0003";
        auto diagnostic = at(source, prefix.name.span, phase ? "LYM0004" : "LYM0002",
            (phase ? "宏的编译期依赖必须是手写且已可用的声明：" : "宏求值失败：") + failure.message);
        if (failure.site) diagnostic.generated = failure.site;
        throw diagnostic;
    }
    auto output = execution.output;
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
    result.importParent = prefix.file;
    return result;
}
}

void expandExpressions(Source& source, std::size_t file, const MacroPackage& package, std::vector<fs::path>& inputs,
    const MacroEnvironment& environment) {
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
            auto generated = expand(source, prefix, package, nullptr, {}, "expression", inputs, environment, &call);
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
}

void expandMacros(Module& syntax, std::vector<Source>& sources, const std::map<std::string, MacroPackage>& packages,
    std::vector<fs::path>& inputs, const MacroTypeResolver& resolveType, std::string_view typeEnvironment, const MacroEnvironment& environment) {
    auto prefixes = std::exchange(syntax.expansions, {});
    std::vector<std::pair<Expansion, ExpandedMacro>> decorators;
    for (const auto& prefix : prefixes) {
        const auto& source = sources[prefix.file];
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
            for (auto slot = static_cast<std::size_t>(declaration.function->hasReceiver); slot < declaration.function->parameters.size(); ++slot) {
                const auto& param = declaration.function->parameters[slot];
                fields.push_back({param.name, param.type, false, true, param.attributes});
            }
        }
        const auto& name = declaration.enumeration ? declaration.enumeration->name : declaration.aggregate ? declaration.aggregate->name : declaration.function->name;
        auto kind = declaration.enumeration ? "enum" : declaration.aggregate ? declaration.aggregate->reference ? "class" : "struct" : "fn";
        auto generated = expand(source, prefix, packages.at(source.moduleId), &name, fields, kind, inputs, environment, nullptr, declaration);
        if (prefix.decorator) {
            if (prefix.kind == ExpansionTarget::Aggregate) {
                if (generated.output.hooks) throw at(source, prefix.name.span, "LYM0002", "类型装饰器应生成辅助声明，不能返回 syntaxAround");
            } else {
                if (!prefix.exports.empty()) throw at(source, prefix.name.span, "LYM0002", "函数装饰器不能生成公开声明");
                if (declaration.function->owner && !declaration.function->hasReceiver)
                    throw at(source, prefix.name.span, "LYM0002", "成员装饰器只适用于实例方法");
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
        if (!syntax.expansions.empty() || std::any_of(syntax.imports.begin() + imports, syntax.imports.end(), [](const auto& use) { return use.isPublic || use.kind != ImportKind::Runtime; })) reject();
        for (const auto& request : prefix.exports) if (!allowed.at(request.text))
            throw at(original, request.span, "LYM0002", "宏没有生成所请求的公开声明：" + request.text);
        storeExpansion(generated);
    }
    for (auto item = decorators.rbegin(); item != decorators.rend(); ++item) {
        auto& [prefix, generated] = *item;
        auto site = prefix.name.span;
        auto start = std::to_string(syntax.functions.size());
        auto hidden = syntax.functions[prefix.target];
        std::size_t enclosing = 0;
        if (hidden.owner) {
            auto owns = [&](const auto& type) { return type.name.text == hidden.owner->text && sources[type.file].moduleId == sources[prefix.file].moduleId; };
            if (auto aggregate = std::ranges::find_if(syntax.aggregates, owns); aggregate != syntax.aggregates.end()) enclosing = aggregate->typeParameters.size();
            else enclosing = std::ranges::find_if(syntax.enums, owns)->typeParameters.size();
        }
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
            if (hidden.hasReceiver) {
                hook.owner = Identifier{hidden.owner->text, {}};
                hook.hasReceiver = true;
                hook.parameters.insert(hook.parameters.begin(), Parameter{{"$this", hook.name.span}, {hidden.parameters.front().type.text, {}}});
                hook.typeParameters = hidden.typeParameters;
                for (auto& type : hook.typeParameters) type.span = {};
                hook.bounds = hidden.bounds;
                for (auto& bound : hook.bounds) {
                    bound.trait.span = {};
                    for (auto& binding : bound.bindings) binding.member.span = binding.type.span = {};
                }
            }
            sources.push_back(std::move(source));
            syntax.functions.push_back(std::move(hook));
        }
        hidden.name.text = "$decorated" + start;
        hidden.name.span = site;
        hidden.isPublic = false;
        hidden.pendingExpansion.reset();
        for (auto& param : hidden.parameters) param.defaultValue.reset();
        auto name = hidden.name.text;
        syntax.functions.push_back(std::move(hidden));
        auto& fn = syntax.functions[prefix.target];
        fn.pendingExpansion.reset();
        auto add = [&](auto node) {
            fn.expressions.push_back({site, std::move(node)});
            return fn.expressions.size() - 1;
        };
        auto block = [&](std::vector<Statement> statements) { return add(Block{std::move(statements), site}); };
        auto call = [&](const std::string& name, std::vector<Argument> args) {
            auto receiver = fn.hasReceiver ? std::optional(add(This{})) : std::nullopt;
            std::vector<Identifier> types;
            if (receiver || name.starts_with("$decorated")) types.assign(fn.typeParameters.begin() + enclosing, fn.typeParameters.end());
            return add(Call{{name, site}, std::move(args), receiver, std::move(types)});
        };
        std::vector<Argument> args;
        for (auto slot = static_cast<std::size_t>(fn.hasReceiver); slot < fn.parameters.size(); ++slot) {
            const auto& param = fn.parameters[slot];
            args.push_back({{}, add(Identifier{param.name.text, param.name.span}), param.variadic ? std::optional(site) : std::nullopt});
        }
        auto value = call(name, std::move(args));
        if (fn.isAsync) value = add(TaskOp{false, site, value});
        if (enabled[2]) {
            auto exception = Identifier{"$decoratorException", site};
            auto failure = call(hooks[2], {{{}, add(exception)}});
            auto rethrow = add(Raise{site, std::nullopt});
            auto observe = add(Deferred{block({{failure, site}})});
            auto handler = block({{observe, site}, {rethrow, {}}});
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
