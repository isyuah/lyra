module;
#include <charconv>
#include <algorithm>
#include <cstdint>
#include <lyra/utf8.hpp>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

module lyra.syntax;

namespace lyra {
namespace {

enum class Kind { Name, Keyword, Escaped, Number, String, Bytes, Char, TextHead, TextMiddle, TextTail, Symbol, Bad, Hole, End };
constexpr std::string_view keywords[] = {"fn", "val", "var", "const", "if", "else", "while", "for", "in", "ref", "return", "break", "continue", "true", "false", "struct", "class", "enum", "trait", "pub", "import", "as", "is", "async", "await", "spawn", "extern", "match", "null", "implicit", "defer", "throw", "try", "catch", "this", "static"};

struct Token {
    Kind kind;
    Span span;
    bool newlineBefore;
    Span content{};
    bool raw{};
};

bool isLetter(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

bool isDigit(char value) { return value >= '0' && value <= '9'; }
bool basedInteger(std::string_view value) { return value.starts_with("0x") || value.starts_with("0o") || value.starts_with("0b"); }

std::uint32_t hexEscape(const Source& source, Span digits, std::size_t maximum) {
    auto word = std::string_view(source.text).substr(digits.begin, digits.end - digits.begin);
    std::uint32_t value{};
    auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value, 16);
    if (word.empty() || word.size() > maximum || error != std::errc{} || end != word.data() + word.size())
        throw at(source, digits, "LYL0003", "无效的十六进制转义");
    return value;
}

std::string decodeLiteral(const Source& source, Token token) {
    auto bytes = std::string_view(source.text).substr(token.content.begin, token.content.end - token.content.begin);
    if (token.raw) return std::string(bytes);
    std::string result;
    for (std::size_t index = token.content.begin; index < token.content.end; ++index) {
        auto byte = source.text[index];
        if (byte == '\\') {
            auto start = index;
            if (++index == token.content.end) throw at(source, {start, index}, "LYL0003", "不完整的字面量转义");
            byte = source.text[index];
            switch (byte) {
                case 'n': byte = '\n'; break;
                case 'r': byte = '\r'; break;
                case 't': byte = '\t'; break;
                case '0': byte = '\0'; break;
                case '\\': case '"': case '\'': case '$': break;
                case 'x': {
                    if (token.kind != Kind::Bytes || token.content.end - index < 3) throw at(source, {start, index + 1}, "LYL0003", "\\xHH 只用于字节串且需要两个十六进制数字");
                    byte = static_cast<char>(hexEscape(source, {index + 1, index + 3}, 2));
                    index += 2;
                    break;
                }
                case 'u': {
                    if (token.kind == Kind::Bytes || index + 1 == token.content.end || source.text[index + 1] != '{')
                        throw at(source, {start, index + 1}, "LYL0003", "Unicode 转义需要 \\u{十六进制标量}");
                    auto close = bytes.find('}', index + 2 - token.content.begin);
                    if (close == bytes.npos) throw at(source, {start, token.content.end}, "LYL0003", "Unicode 转义缺少 }");
                    close += token.content.begin;
                    auto point = hexEscape(source, {index + 2, close}, 6);
                    if (!utf8::scalar(point)) throw at(source, {start, close + 1}, "LYL0003", "转义不是有效的 Unicode 标量");
                    char encoded[4];
                    result.append(encoded, utf8::encode(point, encoded));
                    index = close;
                    continue;
                }
                default: throw at(source, {start, index + 1}, "LYL0003", "未知的字面量转义");
            }
        }
        result.push_back(byte);
    }
    return result;
}

class Lexer {
public:
    explicit Lexer(const Source& source, std::vector<SyntaxToken>* comments = nullptr) : source(source), text(source.text), comments(comments) {}

    Token next() {
        bool newline = false;
        while (offset < text.size()) {
            auto current = text[offset];
            if (current == ' ' || current == '\t' || current == '\r' || current == '\n') {
                newline |= current == '\n' || current == '\r';
                ++offset;
            } else if (text.substr(offset, 2) == "//") {
                auto begin = offset;
                offset = text.find_first_of("\r\n", offset + 2);
                if (offset == text.npos) offset = text.size();
                if (comments) comments->push_back({{begin, offset}, "comment"});
            } else if (text.substr(offset, 2) == "/*") {
                auto end = text.find("*/", offset + 2);
                if (end == text.npos) throw at(source, {offset, offset + 2}, "LYL0001", "块注释缺少 */");
                newline |= text.substr(offset, end - offset).find_first_of("\r\n") != text.npos;
                if (comments) comments->push_back({{offset, end + 2}, "comment"});
                offset = end + 2;
            } else break;
        }
        auto start = offset;
        if (offset == text.size()) {
            if (!interpolations.empty()) throw at(source, {interpolations.back().opening, start}, "LYL0003", "字符串插值缺少结束的 }");
            return {Kind::End, {start, start}, newline};
        }
        if (!interpolations.empty()) {
            if (text[offset] == '{') ++interpolations.back().braces;
            if (text[offset] == '}') {
                if (!interpolations.back().braces) return quoted(start, offset + 1, '"', Kind::String, newline, true);
                --interpolations.back().braces;
            }
        }
        if (text[offset] == '"' || text[offset] == '\'') return quoted(start, start + 1, text[offset], text[offset] == '"' ? Kind::String : Kind::Char, newline);
        if (text.substr(offset).starts_with("b\"")) return quoted(start, start + 2, '"', Kind::Bytes, newline);
        bool bytes = text.substr(offset).starts_with("br");
        if (bytes || text[offset] == 'r') {
            auto open = offset + (bytes ? 2 : 1);
            auto delimiters = open;
            while (open < text.size() && text[open] == '#') ++open;
            if (open < text.size() && text[open] == '"') {
                auto close = text.substr(delimiters, open - delimiters);
                auto end = text.find('"', open + 1);
                while (end != text.npos && !text.substr(end + 1).starts_with(close)) end = text.find('"', end + 1);
                if (end == text.npos) throw at(source, {start, open + 1}, "LYL0003", "原始字面量缺少结束分隔符");
                offset = end + 1 + close.size();
                return {bytes ? Kind::Bytes : Kind::String, {start, offset}, newline, {open + 1, end}, true};
            }
        }
        char current = text[offset++];
        Kind kind;
        if (isDigit(current)) {
            kind = Kind::Number;
            if (!basedInteger(text.substr(start))) {
                auto digits = [&] { while (offset < text.size() && (isDigit(text[offset]) || text[offset] == '_')) ++offset; };
                digits();
                if (offset + 1 < text.size() && text[offset] == '.' && isDigit(text[offset + 1])) { ++offset; digits(); }
                if (offset < text.size() && (text[offset] == 'e' || text[offset] == 'E')) {
                    ++offset;
                    if (offset < text.size() && (text[offset] == '-' || text[offset] == '+')) ++offset;
                    digits();
                }
            }
            while (offset < text.size() && (isLetter(text[offset]) || isDigit(text[offset]))) ++offset;
        } else if (isLetter(current)) {
            while (offset < text.size() && (isLetter(text[offset]) || isDigit(text[offset]))) ++offset;
            auto word = text.substr(start, offset - start);
            kind = std::ranges::find(keywords, word) == std::end(keywords) ? Kind::Name : Kind::Keyword;
        } else if (current == '`') {
            if (offset == text.size() || !isLetter(text[offset])) throw at(source, {start, offset}, "LYL0001", "反引号内需要标识符");
            while (offset < text.size() && (isLetter(text[offset]) || isDigit(text[offset]))) ++offset;
            if (offset == text.size() || text[offset] != '`') throw at(source, {start, offset}, "LYL0001", "标识符缺少结束反引号");
            ++offset;
            kind = Kind::Escaped;
        } else {
            kind = Kind::Symbol;
            constexpr std::string_view doubles[] = {"+=", "-=", "*=", "/=", "%=", "==", "!=", "<=", ">=", "&&", "||", "=>", "?.", "??", "->", "..", "&=", "^=", "|=", "<<", ">>"};
            constexpr std::string_view triples[] = {"...", "..<", "<<=", ">>="};
            if (std::ranges::find(triples, text.substr(start, 3)) != std::end(triples)) offset += 2;
            else if (std::ranges::find(doubles, text.substr(start, 2)) != std::end(doubles)) ++offset;
            else if (std::string_view("()[]{},.:;+-*/%=<>!#@?|&^~").find(current) == text.npos) {
                while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) ++offset;
                throw at(source, {start, offset}, "LYL0001", "此处不是受支持的 Lyra 记号");
            }
        }
        return {kind, {start, offset}, newline};
    }

    Token editorNext(std::vector<Diagnostic>* errors = nullptr) {
        auto before = offset;
        try { return next(); }
        catch (const Diagnostic& error) {
            if (errors) errors->push_back(error);
            auto start = error.site->start.offset;
            auto from = text.substr(start);
            bool toEnd = from.starts_with("/*") || (error.code == "LYL0003" && (from.starts_with('r') || from.starts_with("br")));
            if (toEnd) offset = text.size();
            else if (offset == text.size() && from.starts_with("${")) return {Kind::End, {offset, offset}, false};
            else offset = std::max(offset, error.site->end.offset);
            if (error.code == "LYL0003" && !interpolations.empty()) interpolations.clear();
            auto kind = from.starts_with("/*") ? "comment" : error.code == "LYL0003" ? "string" : "invalid";
            if (comments && std::string_view(kind) != "invalid") comments->push_back({{start, offset}, kind});
            return {Kind::Bad, {start, offset}, text.substr(before, start >= before ? start - before : 0).find_first_of("\r\n") != text.npos};
        }
    }

private:
    const Source& source;
    std::string_view text;
    std::vector<SyntaxToken>* comments;
    std::size_t offset = 0;
    struct Interpolation { std::size_t opening; unsigned braces; };
    std::vector<Interpolation> interpolations{};

    Token quoted(std::size_t start, std::size_t content, char quote, Kind kind, bool newline, bool resumed = false) {
        offset = content;
        while (offset < text.size()) {
            auto byte = text[offset];
            if (byte == quote) {
                if (resumed) { interpolations.pop_back(); kind = Kind::TextTail; }
                auto end = offset++;
                return {kind, {start, offset}, newline, {content, end}};
            }
            if (byte == '\r' || byte == '\n') break;
            if (kind == Kind::String && text.substr(offset, 2) == "${") {
                auto open = offset;
                offset += 2;
                if (resumed) interpolations.back().opening = open;
                else interpolations.push_back({open, 0});
                return {resumed ? Kind::TextMiddle : Kind::TextHead, {start, offset}, newline, {content, open}};
            }
            if (byte == '\\' && offset + 1 < text.size() && text[offset + 1] != '\r' && text[offset + 1] != '\n') ++offset;
            ++offset;
        }
        throw at(source, {start, offset}, "LYL0003", "字面量缺少结束引号");
    }
};

IntegerLiteral integer(const Source& source, Span digits, bool negative) {
    auto word = std::string_view(source.text).substr(digits.begin, digits.end - digits.begin);
    int base = 10;
    if (word.starts_with("0x")) base = 16;
    if (word.starts_with("0o")) base = 8;
    if (word.starts_with("0b")) base = 2;
    if (base != 10) word.remove_prefix(2);
    if (word.empty() || word.front() == '_' || word.back() == '_' || word.find("__") != word.npos) {
        throw at(source, digits, "LYL0002", "无效的整数字面量");
    }
    std::string magnitude;
    for (char digit : word) if (digit != '_') magnitude.push_back(digit);
    std::uint64_t number = 0;
    auto end = magnitude.data() + magnitude.size();
    auto [stopped, error] = std::from_chars(magnitude.data(), end, number, base);
    if (error == std::errc::result_out_of_range) throw at(source, digits, "LYL0002", "整数字面量超出 64 位整数范围");
    if (error != std::errc{} || stopped != end) throw at(source, digits, "LYL0002", "无效的整数字面量");
    return {number, negative};
}

bool realSpelling(std::string_view text) {
    return !basedInteger(text) && (text.find_first_of(".eE") != text.npos || text.ends_with("f32") || text.ends_with("f64"));
}

RealLiteral real(const Source& source, Span span, std::string_view text) {
    RealLiteral result{};
    if (text.ends_with("f32") || text.ends_with("f64")) {
        result.width = text.ends_with("f32") ? 32 : 64;
        text.remove_suffix(3);
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        auto value = text[index];
        if (value != '_') result.digits += value;
        else if (!index || index + 1 == text.size() || !isDigit(text[index - 1]) || !isDigit(text[index + 1]))
            throw at(source, span, "LYL0002", "浮点字面量的 _ 必须在数字之间");
    }
    return result;
}

struct Infix { std::string_view text; int rank; Op op; };
constexpr Infix infix[] = {
    {"=", 1, Op::Set}, {"+=", 1, Op::AddSet}, {"-=", 1, Op::SubSet}, {"*=", 1, Op::MulSet}, {"/=", 1, Op::DivSet}, {"%=", 1, Op::RemSet},
    {"&=", 1, Op::BitAndSet}, {"^=", 1, Op::BitXorSet}, {"|=", 1, Op::BitOrSet}, {"<<=", 1, Op::ShlSet}, {">>=", 1, Op::ShrSet},
    {"||", 3, Op::Or}, {"&&", 4, Op::And}, {"==", 5, Op::Eq}, {"!=", 5, Op::Ne}, {"<", 5, Op::Lt}, {"<=", 5, Op::Le}, {">", 5, Op::Gt}, {">=", 5, Op::Ge},
    {"|", 6, Op::BitOr}, {"^", 7, Op::BitXor}, {"&", 8, Op::BitAnd}, {"<<", 10, Op::Shl}, {">>", 10, Op::Shr},
    {"+", 11, Op::Add}, {"-", 11, Op::Sub}, {"*", 12, Op::Mul}, {"/", 12, Op::Div}, {"%", 12, Op::Rem}
};

struct CompletionScope {
    std::string_view text;
    std::size_t offset;
    SyntaxCompletion& result;
    std::vector<std::size_t> blocks{};
    std::map<std::string, EditorSymbol> names{};
    bool asynchronous{}, receiver{};
    unsigned loops{};

    bool contains(std::size_t block) const { return std::ranges::find(blocks, block) != blocks.end(); }

    void add(const Identifier& name, EditorKind kind = EditorKind::Variable) {
        if (name.text != "_" && !name.text.starts_with('$') && name.span.begin < name.span.end) {
            auto label = std::string(text.substr(name.span.begin, name.span.end - name.span.begin));
            names.insert_or_assign(label, EditorSymbol{label, kind, name.span});
        }
    }

    void add(const Pattern& pattern) {
        if (const auto* name = std::get_if<PatternName>(&pattern.node); name && !name->owner && !name->module && !name->labels && (name->escaped || name->name.text != "None")) add(name->name);
        if (const auto* list = std::get_if<PatternList>(&pattern.node); list && list->rest) add(*list->rest);
        for (const auto& part : pattern.parts) add(part);
    }
};

struct CompletionReached {};

class Parser {
public:
    Parser(const Source& source, std::size_t file, std::vector<ExpressionExpansion>* expressions = nullptr, bool macroImplementation = false,
        const std::vector<Token>* editorTokens = nullptr, CompletionScope* completion = nullptr, RecoveredSyntax* recovery = nullptr)
        : source(source), file(file), lexer(source), editorTokens(editorTokens), token(readToken()), next(readToken()),
          macros(expressions), macroImplementation(macroImplementation), completion(completion), recovery(recovery) {}

    void appendTo(Module& module) {
        while (token.kind != Kind::End) {
            if (completion) completion->names.clear();
            auto start = token.span.begin;
            auto functions = module.functions.size(), expansions = module.expansions.size();
            auto exceptions = module.exceptions.size(), calls = macros ? macros->size() : 0;
            try { appendDeclaration(module); }
            catch (const Diagnostic& error) {
                if (!editorTokens) throw;
                if (completion) completion->result.incomplete = true;
                if (recovery) {
                    recovery->errors.push_back(error);
                    recovery->incompleteDeclaration = true;
                }
                module.functions.resize(functions);
                module.expansions.resize(expansions);
                module.exceptions.resize(exceptions);
                if (macros) macros->resize(calls);
                selfType.clear();
                nodes.clear();
                pendingExpansion.reset();
                recover(start, true);
            }
        }
    }

    void expansionExpression() {
        expression(true);
        if (token.kind != Kind::End) throw at(source, token.span, "LYM0002", "表达式宏需要恰好一个表达式");
    }

private:
    void appendDeclaration(Module& module) {
            completeAt(CompletionPlace::TopLevel);
            auto firstPrefix = prefixes(module);
            bool pub = visibility();
            if (token.kind == Kind::Hole && completion) completion->result.afterPublic = pub;
            completeAt(CompletionPlace::TopLevel);
            if (is("const")) {
                if (firstPrefix != module.expansions.size()) throw at(source, module.expansions[firstPrefix].name.span, "LYM0001", "不能修饰 const 声明");
                auto prefix = consume().span;
                if (is("fn") && !token.newlineBefore) {
                    auto fn = function({}, pub);
                    if (fn.name.text == "main" || fn.result.text.empty() || std::ranges::any_of(fn.parameters, [](const auto& param) { return param.variadic || param.passing != Passing::Value; }))
                        throw at(source, prefix, "LYCT0001", "const fn 需要显式返回类型，且不能是入口函数、变长函数或引用传参函数");
                    fn.constant = ConstKind::Function;
                    fn.header.begin = prefix.begin;
                    module.functions.push_back(std::move(fn));
                } else {
                    auto name = identifier();
                    if (name.text == "main") throw at(source, name.span, "LYCT0001", "main 不能声明为常量");
                    require(":");
                    auto annotation = type();
                    require("=");
                    auto failures = bodyFailures;
                    auto initializer = expression();
                    if (is(";")) consume();
                    else if (token.kind != Kind::End && !token.newlineBefore) throw at(source, token.span, "LYP0001", "常量声明后需要换行或分号");
                    Function constant{file, std::move(name), {}, std::move(annotation), std::exchange(nodes, {}), initializer, {}, pub};
                    constant.constant = ConstKind::Value;
                    constant.header = {prefix.begin, constant.expressions[initializer].span.begin};
                    constant.pendingExpansion = std::exchange(pendingExpansion, {});
                    constant.incompleteBody = bodyFailures != failures;
                    module.functions.push_back(std::move(constant));
                }
                return;
            }
            if (is("trait")) {
                if (firstPrefix != module.expansions.size()) throw at(source, module.expansions[firstPrefix].name.span, "LYM0001", "不能修饰 trait 约束声明");
                module.traits.push_back(trait(pub, module));
                return;
            }
            if (is("type")) {
                if (firstPrefix != module.expansions.size()) throw at(source, module.expansions[firstPrefix].name.span, "LYM0001", "不能修饰类型别名");
                auto start = consume().span.begin;
                TypeAlias alias{file, identifier(), {}, pub};
                std::vector<Bound> bounds;
                typeParameters(alias.typeParameters, bounds);
                if (!bounds.empty()) throw at(source, bounds.front().trait.span, "LYG0001", "透明类型别名自身不声明约束");
                require("=");
                alias.target = type();
                alias.span = {start, alias.target.span.end};
                if (is(";")) consume();
                else if (token.kind != Kind::End && !token.newlineBefore) throw at(source, token.span, "LYP0001", "类型别名后需要换行或分号");
                module.aliases.push_back(std::move(alias));
                return;
            }
            if (is("enum")) {
                for (auto index = firstPrefix; index < module.expansions.size(); ++index) {
                    auto& prefix = module.expansions[index];
                    if (prefix.decorator) throw at(source, prefix.name.span, "LYM0001", "装饰器不适用于 enum 声明");
                    prefix.kind = ExpansionTarget::Enumeration;
                    prefix.target = module.enums.size();
                }
                module.enums.push_back(enumeration(module, pub));
                return;
            }
            if (is("extension")) {
                if (firstPrefix != module.expansions.size()) throw at(source, module.expansions[firstPrefix].name.span, "LYM0001", "此版本不能修饰扩展组声明");
                extension(module, pub);
                return;
            }
            if (is("exception")) {
                if (firstPrefix != module.expansions.size()) throw at(source, module.expansions[firstPrefix].name.span, "LYM0001", "此版本不能修饰异常声明");
                auto start = consume().span.begin;
                auto name = identifier();
                auto end = name.span.end;
                module.exceptions.push_back({file, std::move(name), pub, {start, end}});
                if (is(";")) consume();
                else if (token.kind != Kind::End && !token.newlineBefore) throw at(source, token.span, "LYP0001", "异常声明后需要换行或分号");
                return;
            }
            for (auto index = firstPrefix; index < module.expansions.size(); ++index) {
                auto& prefix = module.expansions[index];
                if (is("import")) throw at(source, prefix.name.span, "LYM0001", "不能修饰 import");
                if (is("struct") || is("class")) prefix.kind = ExpansionTarget::Aggregate;
                prefix.target = prefix.kind == ExpansionTarget::Aggregate ? module.aggregates.size() : module.functions.size();
            }
            if (is("import")) imports(module, pub);
            else if (is("struct") || is("class")) aggregate(module, pub);
            else appendFunction(module, function({}, pub), firstPrefix);
    }

    const Source& source;
    std::size_t file;
    Lexer lexer;
    const std::vector<Token>* editorTokens;
    std::size_t editorIndex{};
    Token token, next;
    std::vector<ExpressionExpansion>* macros;
    bool macroImplementation;
    CompletionScope* completion{};
    RecoveredSyntax* recovery{};
    std::size_t bodyFailures{};
    std::vector<Expr> nodes;
    std::optional<Span> pendingExpansion;
    std::string selfType;
    std::optional<Span> callOpen;

    struct Lookahead {};
    Parser(const Parser& parser, Lookahead) : source(parser.source), file(parser.file), lexer(parser.lexer), editorTokens(parser.editorTokens),
        editorIndex(parser.editorIndex), token(parser.token), next(parser.next), macros(nullptr), macroImplementation(parser.macroImplementation), selfType(parser.selfType) {}

    Token readToken() { return editorTokens ? (*editorTokens)[std::min(editorIndex++, editorTokens->size() - 1)] : lexer.next(); }

    void completeAt(CompletionPlace place) {
        if (token.kind != Kind::Hole || !completion) return;
        completion->result.place = place;
        completion->result.callOpen = callOpen;
        completion->result.asynchronous = completion->asynchronous;
        completion->result.implicitReceiver = completion->receiver;
        throw CompletionReached{};
    }

    void importCompletion(ImportKind kind, const std::vector<Identifier>& path, CompletionPlace place = CompletionPlace::Import) {
        if (token.kind != Kind::Hole || !completion) return;
        completion->result.importKind = kind;
        completion->result.importPath = path;
        completeAt(place);
    }

    void imports(Module& module, bool pub) {
        consume();
        auto kind = ImportKind::Runtime;
        if ((is("macro") || is("decorator")) && !next.newlineBefore &&
            (next.kind == Kind::Name || next.kind == Kind::Escaped || next.kind == Kind::Hole))
            kind = text(consume()) == "macro" ? ImportKind::Macro : ImportKind::Decorator;
        importCompletion(kind, {});
        std::vector<Identifier> path{identifier()};
        std::vector<Import> items;
        auto add = [&](std::optional<Identifier> member) {
            auto alias = member ? *member : path.back();
            if (is("as")) { consume(); alias = identifier(); }
            else if (!token.newlineBefore) completeAt(CompletionPlace::ImportAlias);
            items.push_back({file, pub, path, std::move(alias), kind, std::move(member)});
        };
        bool grouped = false;
        while (is(".")) {
            consume();
            if (kind == ImportKind::Runtime && is("{")) { grouped = true; break; }
            importCompletion(kind, path);
            path.push_back(identifier());
        }
        if (grouped) {
            consume();
            do {
                if (token.kind == Kind::Hole && completion) for (const auto& item : items) completion->result.selectedImports.push_back(*item.member);
                importCompletion(kind, path, CompletionPlace::ImportMember);
                add(identifier());
                if (!is(",")) break;
                consume();
            } while (!is("}"));
            require("}");
        } else if (kind != ImportKind::Runtime) {
            if (path.size() < 2) throw at(source, path.front().span, "LYP0001", "宏和装饰器导入需要包与声明名");
            auto member = std::move(path.back());
            path.pop_back();
            add(std::move(member));
        } else {
            if (pub) throw at(source, path.back().span, "LYP0001", "pub import 需要用 .{名称} 显式重导出声明");
            add({});
        }
        if (is(";")) consume();
        else if (token.kind != Kind::End && !token.newlineBefore) throw at(source, token.span, "LYP0001", "导入后需要换行或分号");
        for (auto& item : items) module.imports.push_back(std::move(item));
    }

    void qualifiedCompletion(CompletionPlace place, const Identifier* name = nullptr) {
        if (token.kind != Kind::Hole || !completion) return;
        if (name) {
            auto bare = [](std::string_view word) { return word.starts_with('`') ? word.substr(1, word.size() - 2) : word; };
            auto shadowed = std::ranges::any_of(completion->names, [&](const auto& entry) {
                return bare(entry.first) == name->text && (entry.second.kind == EditorKind::TypeParameter) == (place != CompletionPlace::Member);
            });
            if (!shadowed) completion->result.qualifier = *name;
        }
        completeAt(place);
    }

    bool startsDeclaration() const {
        bool declarationName = !next.newlineBefore && (next.kind == Kind::Name || next.kind == Kind::Escaped);
        return is("pub") || is("fn") || is("extern") || is("implicit") || is("static") ||
            is("import") || is("struct") || is("class") || is("enum") || is("trait") ||
            (declarationName && (is("extension") || is("type") || is("exception"))) ||
            (!next.newlineBefore && (is("async") || is("const")) && text(next) == "fn");
    }

    void recover(std::size_t start, bool topLevel) {
        unsigned braces = 0;
        auto first = std::ranges::lower_bound(*editorTokens, start, {}, [](const Token& item) { return item.span.begin; });
        for (; first != editorTokens->end() && first->span.begin < token.span.begin; ++first) {
            if (first->kind != Kind::Symbol) continue;
            auto spelling = text(*first);
            if (spelling == "{") ++braces;
            else if (spelling == "}" && braces) --braces;
        }
        while (token.kind != Kind::End) {
            completeAt(topLevel ? CompletionPlace::TopLevel : CompletionPlace::Statement);
            bool nextLine = token.span.begin > start && token.newlineBefore;
            if ((!braces && ((!topLevel && is("}")) || nextLine)) || (nextLine && startsDeclaration())) return;
            if (is("{")) ++braces;
            else if (is("}") && braces) --braces;
            else if (!braces && is(";")) { consume(); return; }
            consume();
        }
    }

    bool completingBlock() const { return completion && is("{") && completion->contains(token.span.begin); }

    std::size_t prefixes(Module& module) {
        auto first = module.expansions.size();
        while (is("@") || is("#")) {
            bool decorator = text(consume()) == "@";
            completeAt(decorator ? CompletionPlace::Decorator : CompletionPlace::DeclarationMacro);
            auto name = identifier();
            std::optional<Identifier> argument;
            std::vector<Identifier> extra, exports;
            if (is("(")) {
                consume();
                if (!is(")") && !is("export")) argument = metadataValue();
                while (!is(")")) {
                    if (argument || !exports.empty()) require(",");
                    if (is("export")) { consume(); exports.push_back(identifier()); }
                    else if (!decorator && name.text == "derive" && exports.empty()) extra.push_back(metadataValue());
                    else throw at(source, token.span, "LYM0001", "此处需要 export name 或结束宏实参");
                }
                require(")");
            }
            module.expansions.push_back({file, name, std::move(argument), decorator, ExpansionTarget::Function, 0, exports});
            for (auto& item : extra) module.expansions.push_back({file, name, std::move(item), decorator, ExpansionTarget::Function, 0, exports});
        }
        return first;
    }

    void appendFunction(Module& module, Function fn, std::size_t firstPrefix) {
        for (auto index = firstPrefix; index < module.expansions.size(); ++index) {
            auto& prefix = module.expansions[index];
            prefix.target = module.functions.size();
            if (prefix.decorator) fn.pendingExpansion = prefix.name.span;
        }
        module.functions.push_back(std::move(fn));
    }

    Token consume() {
        auto previous = token;
        token = next;
        next = readToken();
        return previous;
    }

    std::string_view text(Token item) const { return item.kind == Kind::Hole ? std::string_view{} : std::string_view(source.text).substr(item.span.begin, item.span.end - item.span.begin); }
    bool is(std::string_view word) const { return text(token) == word; }

    Passing passing() {
        if (!is("in") && !is("ref")) return Passing::Value;
        return text(consume()) == "in" ? Passing::In : Passing::Ref;
    }
    Token require(std::string_view word) {
        if (!is(word)) throw at(source, token.span, "LYP0001", "此处需要 " + std::string(word));
        return consume();
    }

    Identifier identifier() {
        completeAt(CompletionPlace::None);
        if (token.kind == Kind::Hole && editorTokens) return {"Cursor", consume().span};
        if (token.kind != Kind::Name && token.kind != Kind::Escaped) throw at(source, token.span, "LYP0001", "此处需要标识符");
        auto item = consume();
        auto word = text(item);
        if (item.kind == Kind::Escaped) word = word.substr(1, word.size() - 2);
        return {std::string(word), item.span};
    }
    Identifier stringLiteral() {
        auto item = consume();
        return {decodeLiteral(source, item), item.span};
    }

    CharLiteral character(Token item) const {
        auto bytes = decodeLiteral(source, item);
        if (bytes.empty() || bytes.size() != utf8::sequenceWidth(static_cast<unsigned char>(bytes.front())))
            throw at(source, item.span, "LYL0003", "字符字面量必须恰好包含一个 Unicode 标量");
        return {utf8::decode(bytes.data(), static_cast<unsigned>(bytes.size()))};
    }

    ExprId textTemplate() {
        auto segment = consume();
        auto begin = segment.span.begin;
        TextTemplate result;
        for (;;) {
            if (segment.content.begin != segment.content.end) result.parts.push_back(add(segment.content, StringLiteral{decodeLiteral(source, segment)}));
            if (segment.kind == Kind::TextTail) break;
            auto value = expression(true);
            result.parts.push_back(add(nodes[value].span, DisplayValue{value}));
            if (token.kind != Kind::TextMiddle && token.kind != Kind::TextTail) throw at(source, token.span, "LYP0001", "插值表达式后需要 }");
            segment = consume();
        }
        return add({begin, segment.span.end}, std::move(result));
    }
    Identifier metadataValue() { return token.kind == Kind::String ? stringLiteral() : identifier(); }
    std::vector<Attribute> attributes() {
        std::vector<Attribute> result;
        while (is("[")) {
            consume();
            Attribute attribute{identifier(), {}};
            if (std::ranges::any_of(result, [&](const auto& other) { return other.name.text == attribute.name.text; }))
                throw at(source, attribute.name.span, "LYM0003", "重复标记：" + attribute.name.text);
            if (is("(")) { consume(); attribute.argument = metadataValue(); require(")"); }
            require("]");
            result.push_back(std::move(attribute));
        }
        return result;
    }
    ExprId add(Span span, auto node) {
        nodes.push_back({span, std::move(node)});
        return nodes.size() - 1;
    }

    Identifier type() {
        completeAt(CompletionPlace::Type);
        if (is("(")) {
            auto start = consume().span.begin;
            std::vector<Identifier> parameters;
            if (!is(")")) {
                parameters.push_back(type());
                while (is(",")) { consume(); parameters.push_back(type()); }
            }
            auto end = require(")").span.end;
            if (is("->")) {
                consume();
                auto result = type();
                std::string key = "$fn<";
                for (const auto& param : parameters) key += param.text + ',';
                return {key + result.text + '>', {start, result.span.end}};
            }
            if (parameters.size() != 1) throw at(source, {start, end}, "LYP0001", "函数类型在形参列表后需要 -> 返回类型");
            auto name = std::move(parameters.front());
            name.span = {start, end};
            return typeSuffixes(std::move(name));
        }
        if (is("Self")) {
            Identifier name{selfType.empty() ? "$Self" : selfType, consume().span};
            if (is("<")) throw at(source, token.span, "LYG0001", "Self 不接收类型实参");
            return typeSuffixes(std::move(name));
        }
        auto name = [&] {
            if (!is("dyn") || next.newlineBefore || (next.kind != Kind::Name && next.kind != Kind::Escaped && next.kind != Kind::Hole))
                return typePath(CompletionPlace::TypeMember);
            auto start = consume().span.begin;
            completeAt(CompletionPlace::Constraint);
            auto trait = typePath(CompletionPlace::ConstraintMember);
            if (is("<")) {
                auto bindings = associatedBindings(trait.span.end);
                std::ranges::sort(bindings, {}, [](const auto& binding) { return binding.member.text; });
                trait.text += '<';
                for (const auto& [member, value] : bindings) {
                    if (!trait.text.ends_with('<')) trait.text += ',';
                    trait.text += "$bind:" + member.text + '<' + value.text + '>';
                }
                trait.text += '>';
            }
            return Identifier{"$dyn:" + trait.text, {start, trait.span.end}};
        }();
        if (!name.text.starts_with("$dyn:") && is("<")) {
            consume();
            name.text += '<' + type().text;
            while (is(",")) { consume(); name.text += ',' + type().text; }
            name.span.end = closeAngle().end;
            name.text += '>';
        } else if (!name.text.starts_with("$dyn:")) {
            if (auto dot = name.text.find('.'); dot != name.text.npos)
                name.text = "$assoc:" + name.text.substr(dot + 1) + '<' + name.text.substr(0, dot) + '>';
        }
        return typeSuffixes(std::move(name));
    }

    Identifier typePath(CompletionPlace place) {
        auto name = identifier();
        if (is(".") && !token.newlineBefore) {
            consume();
            qualifiedCompletion(place, &name);
            auto member = identifier();
            name.text += '.' + member.text;
            name.span.end = member.span.end;
        }
        return name;
    }

    Identifier typeSuffixes(Identifier name) {
        while (is(".") && !token.newlineBefore) {
            consume();
            auto member = identifier();
            name.text = "$assoc:" + member.text + '<' + name.text + '>';
            name.span.end = member.span.end;
        }
        while (is("?")) { name.span.end = consume().span.end; name.text = "Option<" + name.text + ">"; }
        return name;
    }

    std::vector<Identifier> typeList(std::size_t* end = nullptr) {
        require("<");
        std::vector<Identifier> result{type()};
        while (is(",")) { consume(); result.push_back(type()); }
        auto close = closeAngle().end;
        if (end) *end = close;
        return result;
    }

    Span closeAngle() {
        if (is(">>") || is(">>=") || is(">=")) {
            auto begin = token.span.begin++;
            token.newlineBefore = false;
            return {begin, begin + 1};
        }
        return require(">").span;
    }

    std::vector<AssociatedBinding> associatedBindings(std::size_t& end) {
        require("<");
        std::vector<AssociatedBinding> bindings;
        for (;;) {
            auto member = identifier();
            require("=");
            if (std::ranges::any_of(bindings, [&](const auto& binding) { return binding.member.text == member.text; }))
                throw at(source, member.span, "LYG0007", "关联类型重复绑定：" + member.text);
            bindings.push_back({std::move(member), type()});
            if (!is(",")) break;
            consume();
        }
        end = closeAngle().end;
        return bindings;
    }

    Trait trait(bool pub, Module& module) {
        auto start = require("trait").span.begin;
        Trait declaration{file, identifier(), {}, pub};
        require("{");
        while (!is("}")) {
            completeAt(CompletionPlace::TraitDeclaration);
            if (is("type")) {
                consume();
                declaration.associated.push_back(identifier());
            } else {
                auto begin = token.span.begin;
                bool asynchronous = is("async");
                if (asynchronous) consume();
                require("fn");
                auto name = identifier();
                std::vector<Identifier> generics;
                std::vector<Bound> bounds;
                auto previousNames = completion ? std::optional(completion->names) : std::nullopt;
                typeParameters(generics, bounds);
                require("(");
                std::vector<Parameter> parameters;
                while (!is(")")) {
                    auto parameter = identifier(); require(":");
                    parameters.push_back({std::move(parameter), type()});
                    if (!is(",")) break;
                    consume();
                }
                require(")"); require(":");
                auto result = type();
                auto end = result.span.end;
                TraitMethod method{std::move(name), std::move(parameters), std::move(result), asynchronous, {begin, end}, std::move(generics), std::move(bounds)};
                if (is("{")) {
                    if (completingBlock()) {
                        for (const auto& parameter : method.parameters) completion->add(parameter.name, EditorKind::Parameter);
                        completion->receiver = true;
                        completion->asynchronous = asynchronous;
                    }
                    auto failures = bodyFailures;
                    auto body = block();
                    method.span.end = nodes[body].span.end;
                    Identifier helper{"$traitDefault$" + declaration.name.text + "$" + method.name.text, method.name.span};
                    Identifier self{"$Self", {}};
                    Function fn{file, helper, {{{"$this", {}}, self}}, method.result, std::exchange(nodes, {}), body};
                    fn.typeParameters.push_back(self);
                    fn.typeParameters.insert(fn.typeParameters.end(), method.typeParameters.begin(), method.typeParameters.end());
                    fn.bounds.push_back({0, declaration.name});
                    for (auto bound : method.bounds) { ++bound.parameter; fn.bounds.push_back(std::move(bound)); }
                    fn.parameters.insert(fn.parameters.end(), method.parameters.begin(), method.parameters.end());
                    fn.hasReceiver = true;
                    fn.isAsync = asynchronous;
                    fn.header = {begin, end};
                    fn.incompleteBody = bodyFailures != failures;
                    fn.traitOwner = declaration.name;
                    module.functions.push_back(std::move(fn));
                    method.defaultFunction = std::move(helper);
                }
                if (completion) {
                    completion->names = std::move(*previousNames);
                    completion->receiver = completion->asynchronous = false;
                }
                declaration.methods.push_back(std::move(method));
            }
            if (is(";")) consume();
            else if (!is("}") && !token.newlineBefore) throw at(source, token.span, "LYP0001", "结构约束成员之间需要换行或分号");
        }
        declaration.span = {start, consume().span.end};
        return declaration;
    }

    enum class TypeSuffix { None, Call, Qualifier };

    bool explicitTypes(const Identifier& name) const { return typeSuffix(name.span.end) == TypeSuffix::Call; }

    TypeSuffix typeSuffix(std::size_t nameEnd) const {
        if (!is("<") || token.span.begin != nameEnd) return TypeSuffix::None;
        Parser look(*this, Lookahead{});
        std::size_t end;
        try { look.typeList(&end); }
        catch (const Diagnostic&) { return TypeSuffix::None; }
        if (look.is(".")) return TypeSuffix::Qualifier;
        return look.is("(") && look.token.span.begin == end ? TypeSuffix::Call : TypeSuffix::None;
    }

    bool visibility() { if (!is("pub")) return false; consume(); return true; }

    void typeParameters(std::vector<Identifier>& parameters, std::vector<Bound>& bounds) {
        if (!is("<")) return;
        consume();
        for (;;) {
            parameters.push_back(identifier());
            if (completion) completion->add(parameters.back(), EditorKind::TypeParameter);
            if (is(":")) {
                consume();
                for (;;) {
                    completeAt(CompletionPlace::Constraint);
                    Bound bound{parameters.size() - 1, typePath(CompletionPlace::ConstraintMember)};
                    if (is("<")) {
                        std::size_t end;
                        bound.bindings = associatedBindings(end);
                    }
                    bounds.push_back(std::move(bound));
                    if (!is("&")) break;
                    consume();
                }
            }
            if (!is(",")) break;
            consume();
        }
        closeAngle();
    }

    Enumeration enumeration(Module& module, bool pub) {
        auto start = require("enum").span.begin;
        Enumeration result{file, identifier(), {}, pub};
        typeParameters(result.typeParameters, result.bounds);
        selfType = nominalAnnotation(result).text;
        require("{");
        if (is("}") || is(";")) throw at(source, token.span, "LYP0001", "enum 至少声明一个分支");
        while (!is("}") && !is(";")) {
            EnumCase entry{identifier(), {}};
            entry.span = entry.name.span;
            if (is("(")) {
                consume();
                while (!is(")")) {
                    std::optional<Identifier> name;
                    if (text(next) == ":") { name = identifier(); consume(); }
                    entry.fields.push_back({std::move(name), type()});
                    if (!is(",")) break;
                    consume();
                }
                entry.span.end = require(")").span.end;
            }
            result.cases.push_back(std::move(entry));
            if (is(",")) consume();
            else if (!token.newlineBefore) break;
        }
        if (is(";")) {
            consume();
            while (!is("}")) {
                auto firstPrefix = prefixes(module);
                auto publicMember = visibility();
                completeAt(CompletionPlace::EnumMethod);
                if (!is("fn") && !is("async")) throw at(source, token.span, "LYP0001", "enum 分支之后只允许实例方法");
                appendFunction(module, function(result.name, publicMember, result.typeParameters, false, false, result.bounds), firstPrefix);
                if (is(";")) consume();
                else if (!is("}") && !token.newlineBefore) throw at(source, token.span, "LYP0001", "成员之间需要换行或分号");
            }
        }
        result.span = {start, require("}").span.end};
        selfType.clear();
        return result;
    }

    void field(Module& module, Aggregate& record, bool pub, std::vector<Attribute> marks, bool inParens) {
        if (!is("var") && !is("val")) throw at(source, token.span, "LYP0001", "字段声明需要 val 或 var");
        auto start = token.span.begin;
        bool writable = text(consume()) == "var";
        auto name = identifier();
        require(":");
        Field member{std::move(name), type(), writable, pub, std::move(marks)};
        member.span = {start, member.type.span.end};
        if (!inParens && is("get")) {
            if (writable || !member.attributes.empty()) throw at(source, member.name.span, "LYP0001", "计算属性只能使用没有标记的 val");
            module.functions.push_back(propertyGetter(std::move(member.name), std::move(member.type), nominalAnnotation(record), pub,
                record.typeParameters, record.bounds, record.name));
            module.functions.back().header.begin = start;
            return;
        }
        if (is("=")) {
            consume();
            auto failures = bodyFailures;
            auto body = expression(inParens);
            member.span.end = nodes[body].span.end;
            member.defaultValue = FieldDefault{module.functions.size(), std::holds_alternative<EmptyOption>(nodes[body].node)};
            Identifier helper{fieldDefaultName(record.name.text, member.name.text), member.name.span};
            module.functions.push_back({file, std::move(helper), {}, member.type, std::exchange(nodes, {}), body, {}, false, record.typeParameters, false, {}, false, record.bounds});
            module.functions.back().pendingExpansion = std::exchange(pendingExpansion, {});
            module.functions.back().incompleteBody = bodyFailures != failures;
        }
        record.fields.push_back(std::move(member));
    }

    void bindFieldDefaults(Module& module, const Aggregate& record, Function* init) {
        std::vector<Parameter> earlier;
        std::vector<Statement> assignments;
        for (const auto& member : record.fields) {
            if (member.defaultValue) {
                auto& helper = module.functions[member.defaultValue->function];
                helper.parameters = earlier;
                if (init) {
                    auto site = member.name.span;
                    auto append = [&](auto node) { init->expressions.push_back({site, std::move(node)}); return init->expressions.size() - 1; };
                    auto receiver = append(This{});
                    Call call{helper.name, {}, {}, record.typeParameters};
                    for (const auto& parameter : earlier) call.arguments.push_back({{}, append(Member{receiver, parameter.name})});
                    auto field = append(Member{receiver, member.name});
                    auto value = append(std::move(call));
                    assignments.push_back({append(Binary{Op::Set, site, field, value}), site});
                }
            }
            if (!init || member.defaultValue) earlier.push_back({member.name, member.type});
        }
        if (!assignments.empty()) {
            auto& statements = std::get<Block>(init->expressions[init->body].node).statements;
            statements.insert(statements.begin(), assignments.begin(), assignments.end());
        }
    }

    void aggregate(Module& module, bool pub) {
        auto start = consume();
        bool reference = text(start) == "class";
        Aggregate decl{file, identifier(), reference, {}, pub};
        typeParameters(decl.typeParameters, decl.bounds);
        selfType = nominalAnnotation(decl).text;
        bool primary = is("(");
        auto end = decl.name.span.end;
        std::optional<std::size_t> initializer;
        if (primary) {
            consume();
            while (!is(")")) {
                auto marks = attributes();
                auto publicField = visibility();
                if (token.kind == Kind::Hole && completion) completion->result.afterPublic = publicField;
                completeAt(CompletionPlace::FieldDeclaration);
                field(module, decl, publicField, std::move(marks), true);
                if (!is(",")) break;
                consume();
            }
            end = require(")").span.end;
        }
        if (!primary || is("{")) {
            require("{");
            while (!is("}")) {
                auto firstPrefix = prefixes(module);
                auto marks = attributes();
                bool publicMember = visibility();
                if (token.kind == Kind::Hole && completion) completion->result.afterPublic = publicMember;
                completeAt(CompletionPlace::MemberDeclaration);
                if (firstPrefix != module.expansions.size() && !is("fn") && !is("async"))
                    throw at(source, module.expansions[firstPrefix].name.span, "LYM0001", "成员前缀只适用于方法");
                if (is("type")) {
                    if (!marks.empty()) throw at(source, marks.front().name.span, "LYM0003", "此处的标记只适用于字段和形参");
                    consume();
                    auto name = identifier();
                    require("=");
                    decl.associated.push_back({std::move(name), type(), publicMember});
                } else if (is("init")) {
                    if (!reference || primary || initializer)
                        throw at(source, token.span, "LYP0001", "仅没有主构造参数列表的 class 可以声明一个 init 构造函数");
                    if (!marks.empty()) throw at(source, marks.front().name.span, "LYM0003", "此处的标记只适用于字段和形参");
                    initializer = module.functions.size();
                    module.functions.push_back(function(decl.name, publicMember, decl.typeParameters, true, false, decl.bounds));
                } else if (is("fn") || is("async") || is("extern") || is("static")) {
                    if (!marks.empty()) throw at(source, marks.front().name.span, "LYM0003", "此处的标记只适用于字段和形参");
                    bool staticMember = is("static");
                    if (staticMember) consume();
                    appendFunction(module, function(decl.name, publicMember, decl.typeParameters, false, staticMember, decl.bounds), firstPrefix);
                } else field(module, decl, publicMember, std::move(marks), false);
                if (is(";")) consume();
                else if (!is("}") && !token.newlineBefore) throw at(source, token.span, "LYP0001", "成员之间需要换行或分号");
            }
            end = consume().span.end;
        }
        bindFieldDefaults(module, decl, initializer ? &module.functions[*initializer] : nullptr);
        decl.span = {start.span.begin, end};
        selfType.clear();
        module.aggregates.push_back(std::move(decl));
    }

    Function propertyGetter(Identifier name, Identifier result, Identifier receiver, bool pub,
        const std::vector<Identifier>& generics, const std::vector<Bound>& bounds, std::optional<Identifier> owner = {}) {
        require("get");
        require("(");
        require(")");
        auto failures = bodyFailures;
        auto header = Span{name.span.begin, token.span.begin};
        auto body = block();
        std::vector<Parameter> parameters{{{"$this", name.span}, std::move(receiver)}};
        Function function{file, std::move(name), std::move(parameters), std::move(result), std::exchange(nodes, {}), body, std::move(owner), pub,
            generics, false, {}, false, bounds};
        function.hasReceiver = true;
        function.property = true;
        function.pendingExpansion = std::exchange(pendingExpansion, {});
        function.incompleteBody = bodyFailures != failures;
        function.header = header;
        return function;
    }

    void extension(Module& module, bool pub) {
        auto start = require("extension").span.begin;
        Extension decl{file, identifier(), {}, pub};
        typeParameters(decl.typeParameters, decl.bounds);
        require("for");
        decl.receiver = type();
        require("{");
        if (is("}")) throw at(source, token.span, "LYP0001", "扩展组至少声明一个实例方法");
        while (!is("}")) {
            completeAt(CompletionPlace::ExtensionDeclaration);
            if (is("val")) {
                auto begin = consume().span.begin;
                auto name = identifier();
                if (std::ranges::any_of(decl.properties, [&](const auto& property) { return property.name.text == name.text; }))
                    throw at(source, name.span, "LYN0002", "扩展属性重复：" + name.text);
                require(":");
                auto result = type();
                auto getter = module.functions.size();
                auto function = propertyGetter(name, std::move(result), decl.receiver, pub, decl.typeParameters, decl.bounds);
                function.header.begin = begin;
                function.extensionGroup = decl.name;
                module.functions.push_back(std::move(function));
                decl.properties.push_back({std::move(name), module.functions.back().result, getter});
            } else {
                if (!is("fn") && !is("async")) throw at(source, token.span, "LYP0001", "扩展组成员需要声明实例方法或只读属性");
                module.functions.push_back(function({}, pub, decl.typeParameters, false, false, decl.bounds, &decl));
            }
            if (is(";")) consume();
            else if (!is("}") && !token.newlineBefore) throw at(source, token.span, "LYP0001", "成员之间需要换行或分号");
        }
        decl.span = {start, consume().span.end};
        module.extensions.push_back(std::move(decl));
    }

    Function function(std::optional<Identifier> owner, bool pub, const std::vector<Identifier>& enclosing = {}, bool initializer = false, bool staticMember = false, const std::vector<Bound>& enclosingBounds = {}, const Extension* extension = nullptr) {
        auto start = token.span.begin;
        auto failures = bodyFailures;
        auto previousNames = completion ? std::optional(completion->names) : std::nullopt;
        completeAt(CompletionPlace::FunctionModifier);
        bool isAsync = false, foreign = false;
        if (is("async") || is("extern")) {
            isAsync = is("async"); foreign = !isAsync;
            consume();
            completeAt(CompletionPlace::FunctionModifier);
            if (is("async") || is("extern")) throw at(source, token.span, "LYF0001", "声明只能使用一个 async 或 extern 修饰符");
        }
        bool conversion = is("implicit");
        auto keyword = require(initializer ? "init" : conversion ? "implicit" : "fn");
        if (conversion && (owner || extension || isAsync || foreign)) throw at(source, keyword.span, "LYV0001", "implicit 必须是顶层同步转换");
        auto name = initializer ? Identifier{"$init$" + owner->text, keyword.span}
                  : conversion ? Identifier{"$implicit" + std::to_string(file) + "_" + std::to_string(keyword.span.begin), keyword.span} : identifier();
        auto generics = enclosing;
        auto bounds = enclosingBounds;
        if (!initializer) typeParameters(generics, bounds);
        if (foreign && (owner || extension || !generics.empty())) throw at(source, name.span, "LYF0001", "extern 不支持方法或泛型");
        require("(");
        std::vector<Parameter> parameters;
        Identifier ownerType;
        bool hasReceiver = (owner || extension) && !initializer && !staticMember;
        if (extension) parameters.push_back({{"$this", name.span}, extension->receiver});
        else if (initializer || hasReceiver) {
            ownerType = *owner;
            if (!enclosing.empty()) {
                ownerType.text += '<';
                for (std::size_t index = 0; index < enclosing.size(); ++index) {
                    if (index) ownerType.text += ',';
                    ownerType.text += enclosing[index].text;
                }
                ownerType.text += '>';
            }
            if (hasReceiver) parameters.push_back({{"$this", name.span}, std::move(ownerType)});
        }
        while (!is(")")) {
            if (!parameters.empty() && parameters.back().variadic) throw at(source, token.span, "LYP0001", "变长形参 " + parameters.back().name.text + " 必须放在最后");
            if (is("...")) throw at(source, token.span, "LYP0001", "变长形参写在类型之后，例如 values: Int...");
            auto marks = attributes();
            auto modeSite = token.span;
            auto mode = passing();
            if (mode != Passing::Value && (isAsync || foreign || conversion || initializer || (!owner && !extension && name.text == "main")))
                throw at(source, modeSite, "LYT0022", "in/ref 只用于普通同步函数或方法");
            auto parameter = identifier();
            require(":");
            Parameter item{std::move(parameter), type()};
            item.attributes = std::move(marks);
            item.passing = mode;
            if (is("...")) { consume(); item.variadic = true; }
            if (is("=")) {
                if (item.variadic) throw at(source, token.span, "LYP0001", "变长形参不能设置默认值");
                consume(); item.defaultValue = expression(true);
            }
            if (foreign && (item.variadic || item.defaultValue)) throw at(source, item.name.span, "LYF0001", "extern 形参不支持变长或默认值");
            if (mode != Passing::Value && (item.variadic || item.defaultValue))
                throw at(source, modeSite, "LYT0022", "in/ref 形参不能变长或有默认值");
            parameters.push_back(std::move(item));
            if (!is(",")) break;
            consume();
        }
        auto parametersEnd = require(")").span.end;
        if (conversion && (!generics.empty() || parameters.size() != 1 || parameters.front().defaultValue || parameters.front().variadic)) throw at(source, keyword.span, "LYV0001", "implicit 需要一个无默认值的普通形参");
        auto result = Identifier{"", {parametersEnd, parametersEnd}};
        if (initializer) result = std::move(ownerType);
        else if (is(":")) { consume(); result = type(); }
        else if (pub || foreign || conversion || (!owner && !extension && name.text == "main"))
            throw at(source, token.span, "LYP0001", "此声明需要用 : 标注返回类型");
        std::vector<Identifier> throwsTypes;
        if (is("throws")) {
            auto site = consume().span;
            if (foreign) throw at(source, site, "LYE0003", "extern 函数不能声明 throws");
            throwsTypes.push_back(type());
            while (is(",")) { consume(); throwsTypes.push_back(type()); }
        }
        if (foreign) {
            auto end = require(";").span.begin;
            auto symbol = name;
            Function declaration{file, std::move(name), std::move(parameters), std::move(result), {}, 0, {}, pub, {}, false, std::move(symbol)};
            declaration.header = {start, end};
            if (completion) completion->names = std::move(*previousNames);
            return declaration;
        }
        if (completingBlock()) {
            for (const auto& parameter : parameters) completion->add(parameter.name, EditorKind::Parameter);
            for (const auto& generic : generics) completion->add(generic, EditorKind::TypeParameter);
            completion->asynchronous = isAsync;
            completion->receiver = hasReceiver || initializer;
        }
        auto end = token.span.begin;
        auto body = block();
        Function decl{file, std::move(name), std::move(parameters), std::move(result), std::exchange(nodes, {}), body, std::move(owner), pub, std::move(generics), isAsync, {}, conversion, std::move(bounds), initializer, hasReceiver, extension ? std::optional(extension->name) : std::nullopt};
        decl.header = {start, end};
        decl.throwsTypes = std::move(throwsTypes);
        decl.pendingExpansion = std::exchange(pendingExpansion, {});
        decl.incompleteBody = bodyFailures != failures;
        if (completion) completion->names = std::move(*previousNames);
        return decl;
    }

    ExprId block() {
        return blockContents(require("{").span.begin);
    }

    ExprId blockContents(std::size_t start) {
        Block body;
        while (!is("}")) {
            if (token.kind == Kind::End || (editorTokens && token.newlineBefore && startsDeclaration())) {
                auto end = Span{token.span.begin, token.span.begin};
                if (!editorTokens) throw at(source, end, "LYP0001", "块缺少 }");
                if (recovery) {
                    recovery->errors.push_back(at(source, end, "LYP0001", "块缺少 }"));
                    ++bodyFailures;
                }
                body.end = end;
                return add({start, end.end}, std::move(body));
            }
            auto statementStart = token.span.begin, saved = nodes.size();
            auto calls = macros ? macros->size() : 0;
            try {
                auto value = statement();
                std::optional<Span> semi;
                if (is(";")) semi = consume().span;
                else if (!is("}") && token.kind != Kind::End && !token.newlineBefore)
                    throw at(source, token.span, "LYP0001", "表达式之间需要换行或分号");
                if (completion && completion->contains(start) && semi.value_or(nodes[value].span).end <= completion->offset) {
                    if (const auto* binding = std::get_if<Binding>(&nodes[value].node)) completion->add(binding->name, binding->constant ? EditorKind::Constant : EditorKind::Variable);
                    if (const auto* binding = std::get_if<PatternBinding>(&nodes[value].node)) completion->add(binding->pattern);
                }
                body.statements.push_back({value, semi});
            } catch (const Diagnostic& error) {
                if (!editorTokens) throw;
                if (completion) completion->result.incomplete = true;
                if (recovery) {
                    recovery->errors.push_back(error);
                    ++bodyFailures;
                }
                nodes.resize(saved);
                if (macros) macros->resize(calls);
                recover(statementStart, false);
            }
        }
        body.end = consume().span;
        auto span = Span{start, body.end.end};
        return add(span, std::move(body));
    }

    ExprId statement() {
        completeAt(CompletionPlace::Statement);
        auto start = token.span.begin;
        if (is("const")) {
            consume();
            auto name = identifier();
            require(":");
            auto annotation = type();
            require("=");
            auto value = expression();
            return add({start, nodes[value].span.end}, Binding{std::move(name), false, std::move(annotation), value, true});
        }
        if (is("defer")) {
            consume();
            auto body = expression();
            return add({start, nodes[body].span.end}, Deferred{body});
        }
        if (is("val") || is("var")) {
            bool writable = text(consume()) == "var";
            auto initializer = [&] {
                std::optional<Identifier> annotation;
                if (is(":")) { consume(); annotation = type(); }
                require("=");
                return std::pair{std::move(annotation), expression()};
            };
            bool simpleName = (token.kind == Kind::Name || token.kind == Kind::Escaped) && !is("_") && (text(next) == ":" || text(next) == "=");
            if (writable || simpleName) {
                auto name = identifier();
                auto [annotation, value] = initializer();
                return add({start, nodes[value].span.end}, Binding{std::move(name), writable, std::move(annotation), value});
            }
            auto binding = pattern();
            auto [annotation, value] = initializer();
            return add({start, nodes[value].span.end}, PatternBinding{std::move(binding), std::move(annotation), value});
        }
        if (is("return")) {
            auto site = consume().span;
            if (token.newlineBefore || is(";") || is("}")) return add(site, Return{});
            auto value = expression();
            return add({start, nodes[value].span.end}, Return{value});
        }
        if (is("break") || is("continue")) {
            auto item = consume();
            return add(item.span, text(item) == "break" ? Jump::Break : Jump::Continue);
        }
        if (is("while")) {
            consume();
            std::optional<Pattern> matched;
            if (is("let")) {
                consume();
                matched = pattern();
                require("=");
            }
            auto condition = expression(false, 1, false);
            if (matched && completingBlock()) completion->add(*matched);
            if (completingBlock()) ++completion->loops;
            auto body = block();
            return add({start, nodes[body].span.end}, While{condition, body, std::move(matched)});
        }
        if (is("for")) return forLoop();
        return expression();
    }

    Identifier hidden(std::string_view role, Span site) {
        return {"$" + std::string(role) + std::to_string(site.begin), site};
    }

    ExprId forLoop() {
        auto site = consume().span;
        std::optional<Span> awaitSite;
        if (is("await")) awaitSite = consume().span;
        auto item = pattern();
        if (auto* name = std::get_if<PatternName>(&item.node); name && !name->owner && !name->labels) name->escaped = true;
        require("in");
        auto input = expression(false, 1, false);
        if (completingBlock()) completion->add(item);
        if (completingBlock()) ++completion->loops;
        auto body = block();
        return add({site.begin, nodes[body].span.end}, For{std::move(item), input, body, awaitSite});
    }

    ExprId optional(ExprId subject, Identifier payload, ExprId present, ExprId absent, Span site, Span span, OptionalForm form) {
        Pattern some{site, SumTag::Some, {{payload.span, PatternName{std::move(payload)}}}};
        Pattern none{site, SumTag::None};
        return add(span, Match{subject, {{std::move(some), present}, {std::move(none), absent}}, form, site});
    }

    ExprId expression(bool inParens = false, int minRank = 1, bool trailingBlock = true) {
        auto left = prefix(inParens, trailingBlock);
        int previous = 0;
        while (inParens || !token.newlineBefore) {
            completeAt(CompletionPlace::AfterExpression);
            if (is("??") && minRank <= 2) {
                auto site = consume().span;
                auto right = expression(inParens, 2, trailingBlock);
                auto name = hidden("some", site);
                auto result = add(site, name);
                left = optional(left, name, result, right, site, {nodes[left].span.begin, nodes[right].span.end}, OptionalForm::Default);
                continue;
            }
            if ((is("is") || is("as")) && minRank <= 5) {
                if (previous == 5) throw at(source, token.span, "LYP0001", "比较或类型检查不能连写");
                previous = 5;
                auto operation = is("is") ? TypeOperator::Is : TypeOperator::CheckedCast;
                auto site = consume().span;
                if (operation == TypeOperator::CheckedCast) {
                    if (is("?") && token.span.begin == site.end) site.end = consume().span.end;
                    else if (is("dyn")) operation = TypeOperator::InterfaceCast;
                    else throw at(source, site, "LYP0001", "类型转换写作 as? T 或 as dyn Trait");
                }
                auto target = type();
                auto span = Span{nodes[left].span.begin, target.span.end};
                left = add(span, TypeOperation{operation, left, std::move(target), site});
                continue;
            }
            constexpr int rangeRank = 9;
            if ((is("..") || is("..<")) && minRank <= rangeRank) {
                if (previous == rangeRank) throw at(source, token.span, "LYP0001", "区间运算符不能连写");
                previous = rangeRank;
                bool inclusive = text(consume()) == "..";
                auto right = expression(inParens, rangeRank + 1, trailingBlock);
                left = add({nodes[left].span.begin, nodes[right].span.end}, RangeValue{left, right, inclusive});
                continue;
            }
            auto op = std::ranges::find(infix, text(token), &Infix::text);
            if (op == std::end(infix) || op->rank < minRank) break;
            if ((op->rank == 5 || op->rank == 1) && previous == op->rank) throw at(source, token.span, "LYP0001", "比较或赋值不能连写");
            previous = op->rank;
            auto site = consume().span;
            auto right = expression(inParens, op->rank + 1, trailingBlock);
            left = add({nodes[left].span.begin, nodes[right].span.end}, Binary{op->op, site, left, right});
        }
        return left;
    }

    ExprId prefix(bool inParens, bool trailingBlock) {
        if (is("-") || is("!") || is("~")) {
            auto op = consume();
            auto nextToken = token;
            auto operand = prefix(inParens, trailingBlock);
            auto& value = nodes[operand];
            auto span = Span{op.span.begin, value.span.end};
            if (text(op) == "-" && nextToken.kind == Kind::Number && nextToken.span == value.span) {
                if (auto* integer = std::get_if<IntegerLiteral>(&value.node)) integer->negative = true;
                else std::get<RealLiteral>(value.node).digits.insert(0, 1, '-');
                value.span = span;
                return operand;
            }
            return add(span, Unary{text(op) == "!" ? Op::Not : text(op) == "~" ? Op::Complement : Op::Sub, op.span, operand});
        }
        auto value = taskOperand(inParens, trailingBlock);
        bool propagated = false;
        while (is("?") && (inParens || !token.newlineBefore)) {
            auto site = consume().span;
            value = add({nodes[value].span.begin, site.end}, Propagate{value, site});
            propagated = true;
        }
        if (propagated && (is(".") || is("?.") || ((inParens || !token.newlineBefore) && (is("(") || is("[")))))
            throw at(source, token.span, "LYP0001", "继续使用传播后的值时，请给接收者加括号，例如 (value?).member");
        return value;
    }

    ExprId taskOperand(bool inParens, bool trailingBlock) {
        if (!is("await") && !is("spawn")) return postfix(inParens, trailingBlock);
        auto op = consume();
        if (text(op) == "await" && is("select") && text(next) == "{") return select(op.span);
        auto operand = taskOperand(inParens, trailingBlock);
        return add({op.span.begin, nodes[operand].span.end}, TaskOp{text(op) == "spawn", op.span, operand});
    }

    ExprId select(Span await) {
        Select selection{{await.begin, require("select").span.end}, {}};
        require("{");
        if (is("}")) throw at(source, token.span, "LYP0001", "select 至少需要一个分支");
        while (!is("}")) {
            std::optional<Identifier> binding;
            if (is("val")) { consume(); binding = identifier(); require("="); }
            auto operation = expression(false, 1, false);
            require("=>");
            if (binding && completingBlock()) completion->add(*binding);
            auto value = statement();
            if (binding && completion && nodes[value].span.begin <= completion->offset && completion->offset <= nodes[value].span.end) completion->add(*binding);
            selection.arms.push_back({std::move(binding), operation, value});
            if (is(",")) consume();
            else if (!is("}") && !token.newlineBefore) throw at(source, token.span, "LYP0001", "select 分支之间需要换行或逗号");
        }
        auto end = consume().span.end;
        return add({await.begin, end}, std::move(selection));
    }

    ExprId postfix(bool inParens, bool trailingBlock) {
        auto value = tailArgument(atom(inParens, trailingBlock), trailingBlock);
        while (true) {
            if (is("(") && (inParens || !token.newlineBefore)) {
                auto site = token.span;
                value = tailArgument(call(nodes[value].span.begin, {"()", site}, {}, value), trailingBlock);
                continue;
            }
            if (is("[") && (inParens || !token.newlineBefore)) {
                auto site = consume().span;
                auto offset = expression(true);
                auto end = require("]").span.end;
                value = add({nodes[value].span.begin, end}, Index{value, offset, site});
                continue;
            }
            if (!is(".") && !is("?.")) break;
            bool safe = is("?.");
            auto site = consume().span;
            qualifiedCompletion(CompletionPlace::Member, safe ? nullptr : std::get_if<Identifier>(&nodes[value].node));
            auto name = identifier();
            auto begin = nodes[value].span.begin;
            auto subject = value;
            std::optional<Identifier> payload;
            if (safe) { payload = hidden("some", site); value = add(site, *payload); }
            if (!safe && typeSuffix(name.span.end) == TypeSuffix::Qualifier)
                value = typeMember(begin, std::move(name), inParens, trailingBlock, value);
            else if (explicitTypes(name) || (is("(") && (inParens || !token.newlineBefore))) value = call(begin, std::move(name), value);
            else { auto end = name.span.end; value = add({begin, end}, Member{value, std::move(name)}); }
            value = tailArgument(value, trailingBlock);
            if (safe) {
                auto span = nodes[value].span;
                value = optional(subject, std::move(*payload), value, add(site, EmptyOption{}), site, span, OptionalForm::Access);
            }
        }
        return value;
    }

    IntegerLiteral patternInteger(Span& span) {
        bool negative = is("-");
        if (negative) consume();
        if (token.kind != Kind::Number) throw at(source, token.span, "LYP0001", "整数区间模式需要整数字面量端点");
        auto literal = consume();
        span.end = literal.span.end;
        if (realSpelling(text(literal))) throw at(source, {span.begin, span.end}, "LYT0011", "浮点匹配请使用守卫条件");
        return integer(source, literal.span, negative);
    }

    Pattern pattern() {
        auto first = patternAtom();
        if (!is("|")) return first;
        auto begin = first.span.begin;
        std::vector<Pattern> alternatives;
        alternatives.push_back(std::move(first));
        do {
            consume();
            alternatives.push_back(patternAtom());
        } while (is("|"));
        auto end = alternatives.back().span.end;
        return {{begin, end}, PatternAlternatives{}, std::move(alternatives)};
    }

    Pattern patternAtom() {
        auto site = token.span;
        if (is("[")) {
            consume();
            std::vector<Pattern> parts;
            PatternList list{};
            while (!is("]")) {
                if (is("..")) {
                    consume();
                    list.openEnd = true;
                    if (token.kind != Kind::Name && token.kind != Kind::Escaped)
                        throw at(source, token.span, "LYP0001", "列表 rest 模式需要绑定名称");
                    if (is("_")) consume();
                    else list.rest = identifier();
                    if (is(",")) {
                        auto comma = consume().span;
                        if (!is("]")) throw at(source, comma, "LYP0001", "列表 rest 模式必须位于末尾");
                    }
                    break;
                }
                parts.push_back(pattern());
                if (!is(",")) break;
                consume();
                if (is("]")) break;
            }
            auto end = require("]").span.end;
            return {{site.begin, end}, std::move(list), std::move(parts)};
        }
        if (token.kind == Kind::Number || (is("-") && next.kind == Kind::Number)) {
            auto lower = patternInteger(site);
            if (is("..") || is("..<")) {
                bool inclusive = text(consume()) == "..";
                auto upper = patternInteger(site);
                return {site, PatternIntegerRange{lower, upper, inclusive}};
            }
            return {site, lower};
        }
        if (token.kind == Kind::String) {
            auto literal = stringLiteral();
            return {literal.span, StringLiteral{std::move(literal.text)}};
        }
        if (token.kind == Kind::Char) { auto item = consume(); return {item.span, character(item)}; }
        if (is("true") || is("false")) return {site, text(consume()) == "true"};
        if (is("null")) { consume(); return {site, SumTag::None}; }
        bool escaped = token.kind == Kind::Escaped;
        PatternName name{identifier(), {}, {}, escaped};
        if (is(".")) {
            consume();
            name.owner = std::move(name.name);
            name.escaped = token.kind == Kind::Escaped;
            name.name = identifier();
            if (is(".")) {
                consume();
                name.module = std::move(name.owner);
                name.owner = std::move(name.name);
                name.escaped = token.kind == Kind::Escaped;
                name.name = identifier();
            }
        }
        if (!name.owner && !name.escaped && name.name.text == "_") return {site, std::monostate{}};
        site.end = name.name.span.end;
        if (!is("(")) return {site, std::move(name)};
        consume();
        auto& labels = name.labels.emplace();
        std::vector<Pattern> parts;
        while (!is(")")) {
            std::optional<Identifier> label;
            if (text(next) == ":") { label = identifier(); consume(); }
            labels.push_back(std::move(label));
            parts.push_back(pattern());
            if (!is(",")) break;
            consume();
        }
        return {{site.begin, require(")").span.end}, std::move(name), std::move(parts)};
    }

    ExprId tailArgument(ExprId callee, bool allowed) {
        bool async = is("async") && text(next) == "{" && !next.newlineBefore;
        if (!allowed || (!async && !is("{")) || token.newlineBefore) return callee;
        auto begin = nodes[callee].span.begin;
        if (auto name = std::get_if<Identifier>(&nodes[callee].node)) callee = add(nodes[callee].span, Call{*name, {}});
        else if (auto member = std::get_if<Member>(&nodes[callee].node)) callee = add(nodes[callee].span, Call{member->name, {}, member->receiver});
        else if (!std::holds_alternative<Call>(nodes[callee].node)) return callee;
        Lambda literal{{}, {}, 0, true, async};
        auto site = token.span;
        if (async) consume();
        auto open = require("{").span;
        Parser look(*this, Lookahead{});
        auto readHead = [&]() {
            if (look.is("=>")) return true;
            try {
                for (;;) {
                    LambdaParameter parameter{look.identifier(), {}};
                    if (look.is(":")) { look.consume(); parameter.type = look.type(); }
                    literal.parameters.push_back(std::move(parameter));
                    if (!look.is(",")) break;
                    look.consume();
                }
                return look.is("=>");
            } catch (const Diagnostic&) { return false; }
        };
        if (readHead()) {
            literal.implicit = false;
            while (!is("=>")) consume();
            consume();
        } else literal.parameters.clear();
        if (completion && completion->contains(open.begin)) {
            for (const auto& parameter : literal.parameters) completion->add(parameter.name, EditorKind::Parameter);
            completion->asynchronous = async;
            completion->loops = 0;
        }
        literal.body = blockContents(open.begin);
        auto end = nodes[literal.body].span.end;
        auto value = add({site.begin, end}, std::move(literal));
        std::get<Call>(nodes[callee].node).arguments.push_back({{}, value, {}, site});
        nodes[callee].span = {begin, end};
        return callee;
    }

    bool lambdaHead() const {
        if (next.kind != Kind::Name && next.kind != Kind::Escaped && text(next) != ")") return false;
        Parser look(*this, Lookahead{});
        auto current = next;
        unsigned depth = 1;
        while (current.kind != Kind::End) {
            if (text(current) == "(") ++depth;
            if (text(current) == ")" && !--depth) {
                auto after = text(look.readToken());
                return after == "=>" || after == ":";
            }
            current = look.readToken();
        }
        return false;
    }

    ExprId lambda(std::optional<Span> async = {}) {
        auto open = require("(").span;
        auto start = async.value_or(open).begin;
        Lambda literal{{}, {}, 0, false, async.has_value()};
        while (!is(")")) {
            LambdaParameter parameter{identifier(), {}};
            if (is(":")) { consume(); parameter.type = type(); }
            literal.parameters.push_back(std::move(parameter));
            if (!is(",")) break;
            consume();
        }
        require(")");
        if (is(":")) { consume(); literal.result = type(); }
        require("=>");
        if (completingBlock()) {
            for (const auto& parameter : literal.parameters) completion->add(parameter.name, EditorKind::Parameter);
            completion->asynchronous = async.has_value();
            completion->loops = 0;
        }
        literal.body = block();
        auto end = nodes[literal.body].span.end;
        return add({start, end}, std::move(literal));
    }

    ExprId call(std::size_t begin, Identifier name, std::optional<ExprId> receiver, std::optional<ExprId> function = {}, std::optional<TypeQualifier> typeOwner = {}) {
        auto types = is("<") ? typeList() : std::vector<Identifier>{};
        auto open = require("(").span;
        struct CallScope {
            std::optional<Span>& current;
            std::optional<Span> previous;
            ~CallScope() { current = previous; }
        } frame{callOpen, std::exchange(callOpen, open)};
        Call node{std::move(name), {}, receiver, std::move(types), function, std::move(typeOwner)};
        while (!is(")")) {
            if (completion && token.kind == Kind::Hole) completion->result.argumentLabel = true;
            completeAt(CompletionPlace::Expression);
            std::optional<Identifier> label;
            if (text(next) == ":") { label = identifier(); consume(); }
            auto mode = passing();
            std::optional<Span> spread;
            if (is("...")) spread = consume().span;
            if (spread && mode != Passing::Value) throw at(source, *spread, "LYT0022", "in/ref 实参不能展开");
            node.arguments.push_back({std::move(label), expression(true), spread, {}, mode});
            if (!is(",")) break;
            consume();
        }
        auto close = require(")").span;
        return add({begin, close.end}, std::move(node));
    }

    ExprId typeMember(std::size_t start, Identifier name, bool inParens, bool trailingBlock, std::optional<ExprId> moduleReceiver = {}) {
        TypeQualifier owner{std::move(name), typeList(), moduleReceiver};
        require(".");
        completeAt(CompletionPlace::Member);
        auto member = identifier();
        if (explicitTypes(member) || (is("(") && (inParens || !token.newlineBefore))) return call(start, std::move(member), {}, {}, std::move(owner));
        auto end = member.span.end;
        if (trailingBlock && (is("{") || (is("async") && text(next) == "{" && !next.newlineBefore)) && !token.newlineBefore)
            return add({start, end}, Call{std::move(member), {}, {}, {}, {}, std::move(owner)});
        return add({start, end}, QualifiedMember{std::move(owner), std::move(member)});
    }

    ExprId macroExpression() {
        auto begin = require("#").span.begin;
        completeAt(CompletionPlace::ExpressionMacro);
        auto name = identifier();
        if (!macros) throw at(source, name.span, "LYM0001", "此处不能调用表达式宏 #" + name.text);
        auto open = require("(").span;
        ExpressionExpansion expansion{std::move(name), {begin, begin}, {open.end, open.end}, {}};
        while (!is(")")) {
            auto start = token.span.begin;
            std::size_t end = start;
            std::optional<std::string> literal;
            if (token.kind == Kind::String && (text(next) == ")" || text(next) == ",")) {
                auto string = stringLiteral(); end = string.span.end; literal = std::move(string.text);
            } else {
                std::vector<char> delimiters;
                bool followsName = false;
                while (!delimiters.empty() || (!is(",") && !is(")"))) {
                    if (token.kind == Kind::End) throw at(source, open, "LYP0001", "表达式宏缺少结束括号");
                    if (token.kind == Kind::TextHead) delimiters.push_back('"');
                    else if (token.kind == Kind::TextMiddle || token.kind == Kind::TextTail) {
                        if (delimiters.empty() || delimiters.back() != '"') throw at(source, token.span, "LYP0001", "宏实参的插值内分隔符不匹配");
                        if (token.kind == Kind::TextTail) delimiters.pop_back();
                    }
                    else if (is("<") && ((!delimiters.empty() && delimiters.back() == '>') || (followsName && typeSuffix(end) != TypeSuffix::None))) delimiters.push_back('>');
                    else if (!delimiters.empty() && delimiters.back() == '>' && text(token).starts_with('>')) {
                        delimiters.pop_back();
                        end = closeAngle().end;
                        followsName = false;
                        continue;
                    }
                    else if (is("(") || is("[") || is("{")) delimiters.push_back(is("(") ? ')' : is("[") ? ']' : '}');
                    else if (is(")") || is("]") || is("}")) {
                        if (delimiters.empty() || delimiters.back() != text(token).front()) throw at(source, token.span, "LYP0001", "宏实参括号不匹配");
                        delimiters.pop_back();
                    }
                    followsName = token.kind == Kind::Name || token.kind == Kind::Escaped;
                    end = consume().span.end;
                }
                if (start == end) throw at(source, token.span, "LYP0001", "表达式宏缺少实参");
            }
            expansion.arguments.push_back({{start, end}, std::move(literal)});
            if (!is(",")) break;
            consume();
        }
        auto close = require(")").span;
        expansion.argumentsSpan.end = close.begin;
        expansion.span.end = close.end;
        if (!pendingExpansion) pendingExpansion = expansion.name.span;
        macros->push_back(std::move(expansion));
        return add({begin, close.end}, IntegerLiteral{});
    }

    ExprId quotedSyntax() {
        auto site = consume().span;
        auto start = require("{").span.end;
        std::optional<ExprId> result;
        auto host = [&](std::string name, std::vector<Argument> args, Span span) {
            return add(span, Call{{std::move(name), site}, std::move(args)});
        };
        auto append = [&](ExprId part) {
            result = result ? host("$quoteJoin", {{{}, *result}, {{}, part}}, site) : part;
        };
        auto literal = [&](std::size_t end) {
            if (end == start && result) return;
            auto text = add({start, end}, StringLiteral{source.text.substr(start, end - start)});
            append(host("$quoteText", {{{}, text}}, {start, end}));
        };
        unsigned braces = 0, splices = 0;
        while (token.kind != Kind::End) {
            if (is("}") && !braces) {
                auto end = consume().span;
                literal(end.begin);
                nodes[*result].span = {site.begin, end.end};
                return *result;
            }
            if (is("splice") && text(next) == "(") {
                if (++splices > 64) throw at(source, token.span, "LYM0002", "单个 quote 的 splice 数量超过预算（64）");
                literal(consume().span.begin);
                require("(");
                append(expression(true));
                start = require(")").span.end;
                continue;
            }
            if (is("{")) ++braces;
            else if (is("}")) --braces;
            consume();
        }
        throw at(source, site, "LYM0002", "quote 缺少结束花括号");
    }

    ExprId atom(bool inParens, bool trailingBlock) {
        completeAt(CompletionPlace::Expression);
        auto start = token.span.begin;
        if (is("#")) return macroExpression();
        if (macroImplementation && is("quote") && text(next) == "{") return quotedSyntax();
        if (is("async")) {
            auto site = consume().span;
            if (token.newlineBefore || (!is("{") && !is("("))) throw at(source, site, "LYP0001", "异步表达式需要同行的块或 lambda");
            if (is("(")) return lambda(site);
            if (completingBlock()) { completion->asynchronous = true; completion->loops = 0; }
            auto body = block();
            return add({start, nodes[body].span.end}, AsyncBlock{AsyncBlock::Cold, body, site});
        }
        if (is("[")) {
            consume();
            ListLiteral literal;
            while (!is("]")) {
                literal.elements.push_back(expression(true));
                if (!is(",")) break;
                consume();
            }
            auto end = require("]").span.end;
            return add({start, end}, std::move(literal));
        }
        if (token.kind == Kind::String) {
            auto literal = stringLiteral();
            return add(literal.span, StringLiteral{std::move(literal.text)});
        }
        if (token.kind == Kind::Char) { auto item = consume(); return add(item.span, character(item)); }
        if (token.kind == Kind::Bytes) { auto item = consume(); return add(item.span, BytesLiteral{decodeLiteral(source, item)}); }
        if (token.kind == Kind::TextHead) return textTemplate();
        if (token.kind == Kind::Number) {
            auto literal = consume();
            if (realSpelling(text(literal))) return add(literal.span, real(source, literal.span, text(literal)));
            return add(literal.span, integer(source, literal.span, false));
        }
        if (is("true") || is("false")) {
            auto item = consume();
            return add(item.span, text(item) == "true");
        }
        if (is("null") || is("None")) return add(consume().span, EmptyOption{});
        if (is("this")) return add(consume().span, This{});
        if (is("throw")) {
            auto site = consume().span;
            if (token.newlineBefore || is(";") || is("}") || is(")") || is(",")) return add(site, Raise{site, {}});
            auto value = expression(inParens, 1, trailingBlock);
            return add({start, nodes[value].span.end}, Raise{site, value});
        }
        if (is("try")) {
            consume();
            Attempt attempt{block(), {}};
            do {
                auto site = require("catch").span;
                std::optional<CatchBinding> binding;
                std::optional<ExprId> guard;
                if (!is("{")) {
                    auto name = identifier();
                    require(":");
                    binding = CatchBinding{std::move(name), type()};
                    if (is("if")) { consume(); guard = expression(false, 1, false); }
                }
                if (binding && (completingBlock() || (completion && guard && nodes[*guard].span.begin <= completion->offset && completion->offset <= nodes[*guard].span.end)))
                    completion->add(binding->name);
                attempt.arms.push_back({site, std::move(binding), guard, block()});
            } while (is("catch"));
            auto end = nodes[attempt.arms.back().body].span.end;
            return add({start, end}, std::move(attempt));
        }
        if (is("match")) {
            consume();
            Match match{expression(false, 1, false), {}};
            require("{");
            while (!is("}")) {
                auto branch = pattern();
                std::optional<ExprId> guard;
                if (is("if")) { consume(); guard = expression(false, 1, false); }
                require("=>");
                if (completingBlock() || (completion && guard && nodes[*guard].span.begin <= completion->offset && completion->offset <= nodes[*guard].span.end))
                    completion->add(branch);
                auto value = statement();
                if (completion && nodes[value].span.begin <= completion->offset && completion->offset <= nodes[value].span.end) completion->add(branch);
                match.arms.push_back({std::move(branch), value, guard});
                if (!is(",")) break;
                consume();
            }
            auto end = require("}").span.end;
            return add({start, end}, std::move(match));
        }
        if (is("if")) {
            consume();
            if (is("let")) {
                consume();
                auto matched = pattern();
                require("=");
                auto subject = expression(false, 1, false);
                if (completingBlock()) completion->add(matched);
                auto yes = block();
                std::optional<ExprId> no;
                if (is("else")) { consume(); no = is("if") ? atom(false, trailingBlock) : block(); }
                if (!no) {
                    auto point = nodes[yes].span.end;
                    no = add({point, point}, Block{{}, {point, point}});
                }
                auto end = nodes[*no].span.end;
                auto point = nodes[yes].span.end;
                Match match{subject, {
                    {std::move(matched), yes},
                    {Pattern{{point, point}, std::monostate{}}, *no},
                }};
                return add({start, end}, std::move(match));
            }
            auto condition = expression(false, 1, false);
            auto yes = block();
            std::optional<ExprId> no;
            if (is("else")) { consume(); no = is("if") ? atom(false, trailingBlock) : block(); }
            return add({start, nodes[no.value_or(yes)].span.end}, Conditional{condition, yes, no});
        }
        if (trailingBlock && is("blocking") && text(next) == "{" && !next.newlineBefore) {
            consume();
            if (completingBlock()) { completion->asynchronous = false; completion->loops = 0; }
            auto body = block();
            return add({start, nodes[body].span.end}, Blocking{body});
        }
        if (trailingBlock && is("unsafe") && text(next) == "{" && !next.newlineBefore) {
            consume();
            auto body = block();
            return add({start, nodes[body].span.end}, UnsafeBlock{body});
        }
        if (trailingBlock && (is("taskGroup") || is("parallel")) && text(next) == "{" && !next.newlineBefore) {
            auto kind = is("parallel") ? AsyncBlock::Parallel : AsyncBlock::Group;
            auto site = consume().span;
            if (completingBlock()) { completion->asynchronous = true; completion->loops = 0; }
            auto body = block();
            return add({start, nodes[body].span.end}, AsyncBlock{kind, body, site});
        }
        if (is("{")) return block();
        if (token.kind == Kind::Name || token.kind == Kind::Escaped) {
            auto name = identifier();
            auto suffix = typeSuffix(name.span.end);
            if (suffix == TypeSuffix::Qualifier) return typeMember(start, std::move(name), inParens, trailingBlock);
            if (suffix != TypeSuffix::Call && (!is("(") || (token.newlineBefore && !inParens))) { auto span = name.span; return add(span, std::move(name)); }
            return call(start, std::move(name), {});
        }
        if (is("(")) {
            if (lambdaHead()) return lambda();
            consume();
            auto value = expression(true);
            auto close = require(")").span;
            nodes[value].span = {start, close.end};
            return value;
        }
        throw at(source, token.span, "LYP0001", "此处需要表达式");
    }
};

}

Module parse(const std::vector<Source>& sources) {
    Module functions;
    for (std::size_t index = 0; index < sources.size(); ++index) Parser(sources[index], index).appendTo(functions);
    return functions;
}

void parseFile(Module& module, const Source& source, std::size_t file, std::vector<ExpressionExpansion>* expressions, bool macroImplementation) {
    Parser(source, file, expressions, macroImplementation).appendTo(module);
}

void validateMacroExpression(const Source& generated) {
    try { Parser(generated, 0).expansionExpression(); }
    catch (Diagnostic& failure) { failure.code = "LYM0002"; throw; }
}

std::vector<SyntaxToken> syntaxTokens(const Source& source, bool comments, bool partial) {
    std::vector<SyntaxToken> tokens;
    Lexer lexer(source, comments ? &tokens : nullptr);
    auto next = [&] { return partial ? lexer.editorNext() : lexer.next(); };
    for (auto token = next(); token.kind != Kind::End; token = next()) {
        if (token.kind == Kind::Bad) continue;
        auto kind = token.kind == Kind::Name || token.kind == Kind::Escaped ? "name" : token.kind == Kind::Keyword ? "keyword" :
            token.kind == Kind::String || token.kind == Kind::Bytes || token.kind == Kind::Char || token.kind == Kind::TextHead ||
                token.kind == Kind::TextMiddle || token.kind == Kind::TextTail ? "string" : token.kind == Kind::Number ? "number" : "token";
        tokens.push_back({token.span, kind});
    }
    return tokens;
}

SyntaxCompletion completeSyntax(const Source& source, std::size_t offset) {
    SyntaxCompletion result{{offset, offset}};
    CompletionScope scope{source.text, offset, result};
    const auto text = scope.text;
    std::vector<SyntaxToken> trivia;
    Lexer lexer(source, &trivia);
    std::vector<Token> tokens;
    bool inserted = false;
    for (auto token = lexer.editorNext();; token = lexer.editorNext()) {
        const auto span = token.span;
        if (!inserted && offset >= span.begin && offset <= span.end &&
            (token.kind == Kind::Name || token.kind == Kind::Keyword || token.kind == Kind::Escaped ||
             (token.kind == Kind::Bad && text[span.begin] == '`'))) {
            result.replacement = span;
            tokens.push_back({Kind::Hole, span, token.newlineBefore});
            inserted = true;
            continue;
        }
        if (!inserted && offset > span.begin && offset < span.end) return result;
        if (!inserted && offset <= span.begin) {
            auto start = tokens.empty() ? 0 : tokens.back().span.end;
            auto newline = text.substr(start, offset - start).find_first_of("\r\n") != text.npos;
            tokens.push_back({Kind::Hole, result.replacement, newline});
            inserted = true;
        }
        if (!inserted && token.kind == Kind::Symbol) {
            auto spelling = text.substr(span.begin, span.end - span.begin);
            if (spelling == "{") scope.blocks.push_back(span.begin);
            else if (spelling == "}" && !scope.blocks.empty()) scope.blocks.pop_back();
        }
        tokens.push_back(token);
        if (token.kind == Kind::End) break;
    }
    for (const auto& item : trivia) if (item.span.begin <= offset &&
        (offset < item.span.end || (offset == item.span.end && (item.kind == "string" ||
            text.substr(item.span.begin).starts_with("//") || !text.substr(item.span.begin, item.span.end - item.span.begin).ends_with("*/"))))) return result;
    Module module;
    std::vector<ExpressionExpansion> expansions;
    auto directory = source.path.parent_path().filename();
    bool macroImplementation = macroSourceDirectory(directory);
    try { Parser(source, 0, &expansions, macroImplementation, &tokens, &scope).appendTo(module); }
    catch (const CompletionReached&) {}
    auto offer = [&](std::initializer_list<std::string_view> words) {
        for (auto word : words) result.suggestions.push_back({std::string(word), EditorKind::Keyword});
    };
    using enum CompletionPlace;
    switch (result.place) {
        case TopLevel:
            offer({"fn", "const", "async", "extern", "implicit", "struct", "class", "enum", "trait", "type", "exception", "extension", "import"});
            if (!result.afterPublic) offer({"pub"});
            break;
        case MemberDeclaration:
            offer({"fn", "async", "static", "type", "val", "var", "init"});
            if (!result.afterPublic) offer({"pub"});
            break;
        case FieldDeclaration:
            offer({"val", "var"});
            if (!result.afterPublic) offer({"pub"});
            break;
        case TraitDeclaration: offer({"fn", "async", "type"}); break;
        case ExtensionDeclaration: offer({"fn", "async", "val"}); break;
        case EnumMethod: offer({"fn", "async", "pub"}); break;
        case FunctionModifier: offer({"fn"}); break;
        case Import:
            if (result.importPath.empty() && result.importKind == ImportKind::Runtime) offer({"macro", "decorator"});
            break;
        case ImportAlias: offer({"as"}); break;
        case Type: offer({"dyn"}); break;
        case AfterExpression: offer({"as", "is"}); break;
        case Statement:
            offer({"val", "var", "const", "defer", "return", "for", "while"});
            if (scope.loops) offer({"break", "continue"});
            [[fallthrough]];
        case Expression:
            offer({"if", "match", "async", "true", "false", "null", "throw", "try", "blocking", "unsafe"});
            if (scope.asynchronous) offer({"await", "spawn", "taskGroup", "parallel"});
            if (scope.receiver) offer({"this"});
            if (macroImplementation) offer({"quote"});
            break;
        default: break;
    }
    if (result.place == Statement || result.place == Expression || result.place == Type || result.place == Constraint)
        for (auto& [name, symbol] : scope.names)
            if ((symbol.kind == EditorKind::TypeParameter) == (result.place == Type)) result.suggestions.push_back(std::move(symbol));
    return result;
}

Module indexFileSyntax(const Source& source, std::vector<ExpressionExpansion>* expressions) {
    if (source.previous || source.origin) return indexFileSyntax(sourceText(source.path, source.text, source.moduleId), expressions);
    Lexer lexer(source);
    std::vector<Token> tokens;
    do { tokens.push_back(lexer.editorNext()); } while (tokens.back().kind != Kind::End);
    Module module;
    std::vector<ExpressionExpansion> expansions;
    auto directory = source.path.parent_path().filename();
    auto macro = macroSourceDirectory(directory) ||
        directory == "macros" || directory == "expressions" || directory == "decorators";
    Parser(source, 0, expressions ? expressions : &expansions, macro, &tokens).appendTo(module);
    return module;
}

std::vector<OutlineSymbol> editorOutline(const Source& source, const Module& syntax) {
    std::vector<OutlineSymbol> result;
    std::map<std::string, std::size_t> owners;
    auto symbol = [&](const Identifier& name, EditorKind kind, Span extent) -> OutlineSymbol {
        return {editorSpelling(source, name), kind, name.span,
            {std::min(extent.begin, name.span.begin), std::max(extent.end, name.span.end)}};
    };
    auto container = [&](const auto& declaration, EditorKind kind) -> OutlineSymbol& {
        owners.try_emplace(declaration.name.text, result.size());
        return result.emplace_back(symbol(declaration.name, kind, declaration.span));
    };
    for (const auto& record : syntax.aggregates) {
        auto& children = container(record, record.reference ? EditorKind::Class : EditorKind::Struct).children;
        for (const auto& field : record.fields) children.push_back(symbol(field.name, EditorKind::Field, field.span));
        for (const auto& member : record.associated) children.push_back(symbol(member.name, EditorKind::TypeAlias, {member.name.span.begin, member.type.span.end}));
    }
    for (const auto& item : syntax.enums) {
        auto& children = container(item, EditorKind::Enum).children;
        for (const auto& entry : item.cases) children.push_back(symbol(entry.name, EditorKind::EnumCase, entry.span));
    }
    for (const auto& trait : syntax.traits) {
        auto& children = container(trait, EditorKind::Trait).children;
        for (const auto& method : trait.methods) children.push_back(symbol(method.name, EditorKind::Method, method.span));
        for (const auto& member : trait.associated) children.push_back(symbol(member, EditorKind::TypeAlias, member.span));
    }
    for (const auto& group : syntax.extensions) container(group, EditorKind::Extension);
    for (const auto& alias : syntax.aliases) result.push_back(symbol(alias.name, EditorKind::TypeAlias, alias.span));
    for (const auto& exception : syntax.exceptions) result.push_back(symbol(exception.name, EditorKind::Class, exception.span));
    auto directory = source.path.parent_path().filename();
    auto topFunction = directory == ".decorators" ? EditorKind::Decorator :
        macroSourceDirectory(directory) ? EditorKind::Macro : EditorKind::Function;
    for (const auto& fn : syntax.functions) {
        if ((!fn.initializer && !fn.conversion && fn.name.text.contains('$')) || fn.name.text == "_") continue;
        auto where = fn.header;
        if (!fn.expressions.empty()) where.end = fn.expressions[fn.body].span.end;
        else if (where.end < source.text.size() && source.text[where.end] == ';') ++where.end;
        const auto& owner = fn.owner ? fn.owner : fn.extensionGroup;
        auto kind = fn.constant == ConstKind::Value ? EditorKind::Constant : fn.initializer ? EditorKind::Constructor :
            fn.property ? EditorKind::Property : owner ? EditorKind::Method : topFunction;
        if (!owner) result.push_back(symbol(fn.name, kind, where));
        else if (auto found = owners.find(owner->text); found != owners.end())
            result[found->second].children.push_back(symbol(fn.name, kind, where));
    }
    for (auto& item : result) std::ranges::stable_sort(item.children, {}, &OutlineSymbol::selection);
    std::ranges::stable_sort(result, {}, &OutlineSymbol::selection);
    return result;
}

RecoveredSyntax recoverFileSyntax(const Source& source, std::vector<ExpressionExpansion>* expressions) {
    RecoveredSyntax result;
    std::vector<ExpressionExpansion> calls;
    auto* parsedCalls = expressions ? &calls : nullptr;
    std::optional<Diagnostic> firstError;
    try { parseFile(result.syntax, source, 0, parsedCalls); }
    catch (const Diagnostic& error) { firstError = error; }
    if (firstError) {
        result.syntax = {};
        calls.clear();
        auto unmapped = source.previous || source.origin ? std::optional(sourceText(source.path, source.text, source.moduleId)) : std::nullopt;
        const auto& input = unmapped ? *unmapped : source;
        Lexer lexer(input);
        std::vector<Diagnostic> lexical;
        std::vector<Token> tokens;
        do { tokens.push_back(lexer.editorNext(&lexical)); } while (tokens.back().kind != Kind::End);
        Parser(input, 0, parsedCalls, false, &tokens, nullptr, &result).appendTo(result.syntax);
        std::set<std::size_t> lexicalStarts;
        for (const auto& error : lexical) lexicalStarts.insert(error.site->start.offset);
        std::erase_if(result.errors, [&](const Diagnostic& error) { return lexicalStarts.contains(error.site->start.offset); });
        for (auto& error : lexical) result.errors.push_back(std::move(error));
        std::ranges::stable_sort(result.errors, {}, [](const Diagnostic& error) { return error.site->start.offset; });
        std::set<ExprId> reportedBodies;
        std::erase_if(result.errors, [&](const Diagnostic& error) {
            for (std::size_t index = 0; index < result.syntax.functions.size(); ++index) {
                auto& fn = result.syntax.functions[index];
                if (fn.expressions.empty()) continue;
                auto body = fn.expressions[fn.body].span;
                auto offset = error.site->start.offset;
                if ((body.begin <= offset && offset < body.end) || (offset == body.end && error.site->end.offset == offset && fn.incompleteBody)) {
                    fn.incompleteBody = true;
                    return !reportedBodies.insert(index).second;
                }
            }
            return false;
        });
        if (unmapped) for (auto& error : result.errors)
            error = at(source, {error.site->start.offset, error.site->end.offset}, std::move(error.code), std::move(error.message));
        if (result.errors.empty()) {
            result.errors.push_back(std::move(*firstError));
            result.incompleteDeclaration = true;
        }
        std::erase_if(calls, [&](const ExpressionExpansion& call) {
            return std::ranges::any_of(result.syntax.functions, [&](const Function& fn) {
                return fn.incompleteBody && fn.name.span.begin <= call.span.begin && call.span.end <= fn.expressions[fn.body].span.end;
            });
        });
    }
    if (expressions) for (auto& call : calls) expressions->push_back(std::move(call));
    return result;
}

namespace {

std::string_view trimmed(std::string_view text) {
    auto start = text.find_first_not_of(" \t\r\n");
    return start == text.npos ? std::string_view{} : text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
}

std::string_view excerpt(const Source& source, Span span) {
    return std::string_view(source.text).substr(span.begin, span.end - span.begin);
}

}

std::string editorSpelling(const Source& source, const Identifier& name) {
    if (name.span.begin == name.span.end) return name.text;
    return std::string(excerpt(source, name.span));
}

std::string editorIdentifier(std::string_view name) {
    if (name.empty() || !isLetter(name.front()) || !std::ranges::all_of(name, [](auto letter) { return isLetter(letter) || isDigit(letter); })) return {};
    return std::ranges::find(keywords, name) == std::end(keywords) ? std::string(name) : "`" + std::string(name) + "`";
}

std::string editorModuleAlias(std::string_view name) {
    if (name == "None" || name == "Self") return "`" + std::string(name) + '`';
    return editorIdentifier(name);
}

std::string editorDocumentation(const Source& source, std::size_t start) {
    auto line = std::upper_bound(source.lineStarts.begin(), source.lineStarts.end(), start) - source.lineStarts.begin() - 1;
    auto preceding = [&] { return trimmed(excerpt(source, {source.lineStarts[line - 1], source.lineStarts[line]})); };
    while (line && (preceding().starts_with('@') || preceding().starts_with('#') || preceding().starts_with('['))) --line;
    std::vector<std::string_view> lines;
    while (line && preceding().starts_with("///")) { lines.push_back(trimmed(preceding().substr(3))); --line; }
    std::string documentation;
    for (auto item = lines.rbegin(); item != lines.rend(); ++item) {
        if (item != lines.rbegin()) documentation += '\n';
        documentation += *item;
    }
    return documentation;
}

EditorSignature describeFunction(const Source& source, const Function& function, std::string_view label,
    const std::vector<std::string>& parameterTypes, std::string_view resultType, std::size_t enclosingTypes) {
    EditorSignature signature{{}, editorDocumentation(source, function.header.end ? function.header.begin : function.name.span.begin), {}};
    if (function.isPublic) signature.label += "pub ";
    if (function.constant == ConstKind::Function) signature.label += "const ";
    if (function.owner && !function.hasReceiver && !function.initializer) signature.label += "static ";
    if (function.isAsync) signature.label += "async ";
    if (function.foreignSymbol) signature.label += "extern ";
    if (!function.initializer) signature.label += "fn ";
    signature.label += label;
    if (function.typeParameters.size() > enclosingTypes) {
        signature.label += '<';
        for (const auto& parameter : std::span(function.typeParameters).subspan(enclosingTypes)) {
            if (!signature.label.ends_with('<')) signature.label += ", ";
            signature.label += parameter.text;
        }
        signature.label += '>';
    }
    signature.label += '(';
    for (std::size_t slot = function.hasReceiver; slot < function.parameters.size(); ++slot) {
        const auto& parameter = function.parameters[slot];
        auto name = parameter.name.text.empty() ? std::string{} : editorSpelling(source, parameter.name);
        auto type = parameterTypes.empty() ? trimmed(excerpt(source, parameter.type.span)) : std::string_view(parameterTypes[slot]);
        std::string rendered = name.empty() ? std::string(type) : name + ": " + std::string(type);
        if (parameter.passing != Passing::Value) rendered = std::string(passingName(parameter.passing)) + ' ' + rendered;
        if (parameter.variadic) rendered += "...";
        if (parameter.defaultValue) rendered += " = …";
        if (!signature.parameters.empty()) signature.label += ", ";
        signature.parameters.push_back({std::move(name), rendered, parameter.variadic});
        signature.label += rendered;
    }
    signature.label += ')';
    if (resultType.empty() && function.result.span.begin < function.result.span.end)
        resultType = trimmed(excerpt(source, function.result.span));
    if (!resultType.empty()) signature.label += ": " + std::string(resultType);
    if (!function.throwsTypes.empty()) {
        signature.label += " throws ";
        for (const auto& type : function.throwsTypes) {
            if (&type != &function.throwsTypes.front()) signature.label += ", ";
            signature.label += trimmed(excerpt(source, type.span));
        }
    }
    return signature;
}

}
