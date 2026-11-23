module;
#include <charconv>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

module lyra.syntax;

namespace lyra {
namespace {

enum class Kind { Name, Keyword, Escaped, Number, String, Symbol, End };

struct Token {
    Kind kind;
    Span span;
    bool newlineBefore;
};

bool isLetter(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

bool isDigit(char value) { return value >= '0' && value <= '9'; }

class Lexer {
public:
    explicit Lexer(const Source& source) : source(source), text(source.text) {}

    Token next() {
        bool newline = false;
        while (offset < text.size()) {
            auto current = text[offset];
            if (current == ' ' || current == '\t' || current == '\r' || current == '\n') {
                newline |= current == '\n' || current == '\r';
                ++offset;
            } else if (text.substr(offset, 2) == "//") {
                offset = text.find_first_of("\r\n", offset + 2);
                if (offset == text.npos) offset = text.size();
            } else if (text.substr(offset, 2) == "/*") {
                auto end = text.find("*/", offset + 2);
                if (end == text.npos) throw at(source, {offset, offset + 2}, "LYL0001", "块注释缺少 */");
                newline |= text.substr(offset, end - offset).find_first_of("\r\n") != text.npos;
                offset = end + 2;
            } else break;
        }
        auto start = offset;
        if (offset == text.size()) return {Kind::End, {start, start}, newline};
        char current = text[offset++];
        Kind kind;
        if (isLetter(current) || isDigit(current)) {
            while (offset < text.size() && (isLetter(text[offset]) || isDigit(text[offset]))) ++offset;
            constexpr std::string_view keywords[] = {"fn", "val", "var", "if", "else", "while", "return", "break", "continue", "true", "false", "struct", "class", "pub", "import", "as", "async", "await", "spawn", "extern", "match", "null", "implicit", "defer"};
            auto word = text.substr(start, offset - start);
            kind = isDigit(current) ? Kind::Number : std::ranges::find(keywords, word) == std::end(keywords) ? Kind::Name : Kind::Keyword;
        } else if (current == '"') {
            kind = Kind::String;
            while (offset < text.size() && text[offset] != '"') {
                if (text[offset] == '\r' || text[offset] == '\n') throw at(source, {start, offset}, "LYL0003", "字符串缺少结束引号");
                if (text[offset] == '$' && text.substr(offset, 2) == "${") throw at(source, {offset, offset + 2}, "LYL0003", "此版本尚不支持字符串插值");
                if (text[offset++] == '\\') {
                    if (offset == text.size() || std::string_view("\\\"$nrt0").find(text[offset]) == text.npos) throw at(source, {offset - 1, offset}, "LYL0003", "未知的字符串转义");
                    ++offset;
                }
            }
            if (offset == text.size()) throw at(source, {start, offset}, "LYL0003", "字符串缺少结束引号");
            ++offset;
        } else if (current == '`') {
            if (offset == text.size() || !isLetter(text[offset])) throw at(source, {start, offset}, "LYL0001", "反引号内需要标识符");
            while (offset < text.size() && (isLetter(text[offset]) || isDigit(text[offset]))) ++offset;
            if (offset == text.size() || text[offset] != '`') throw at(source, {start, offset}, "LYL0001", "标识符缺少结束反引号");
            ++offset;
            kind = Kind::Escaped;
        } else {
            kind = Kind::Symbol;
            constexpr std::string_view doubles[] = {"+=", "-=", "*=", "/=", "%=", "==", "!=", "<=", ">=", "&&", "||", "=>", "?.", "??"};
            if (text.substr(start, 3) == "...") offset += 2;
            else if (std::ranges::find(doubles, text.substr(start, 2)) != std::end(doubles)) ++offset;
            else if (std::string_view("()[]{},.:;+-*/%=<>!#@?").find(current) == text.npos) {
                while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) ++offset;
                throw at(source, {start, offset}, "LYL0001", "此处不是受支持的 Lyra 记号");
            }
        }
        return {kind, {start, offset}, newline};
    }

private:
    const Source& source;
    std::string_view text;
    std::size_t offset = 0;
};

std::int64_t integer(const Source& source, Span digits, bool negative) {
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
    constexpr auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (error == std::errc::result_out_of_range || number > limit + negative) {
        throw at(source, digits, "LYL0002", "整数字面量超出 64 位 Int 的范围");
    }
    if (error != std::errc{} || stopped != end) throw at(source, digits, "LYL0002", "无效的整数字面量");
    if (number == limit + 1) return std::numeric_limits<std::int64_t>::min();
    return negative ? -static_cast<std::int64_t>(number) : static_cast<std::int64_t>(number);
}

struct Infix { std::string_view text; int rank; Op op; };
constexpr Infix infix[] = {
    {"=", 1, Op::Set}, {"+=", 1, Op::AddSet}, {"-=", 1, Op::SubSet}, {"*=", 1, Op::MulSet}, {"/=", 1, Op::DivSet}, {"%=", 1, Op::RemSet},
    {"||", 2, Op::Or}, {"&&", 3, Op::And}, {"==", 4, Op::Eq}, {"!=", 4, Op::Ne}, {"<", 4, Op::Lt}, {"<=", 4, Op::Le}, {">", 4, Op::Gt}, {">=", 4, Op::Ge},
    {"+", 5, Op::Add}, {"-", 5, Op::Sub}, {"*", 6, Op::Mul}, {"/", 6, Op::Div}, {"%", 6, Op::Rem}
};

class Parser {
public:
    Parser(const Source& source, std::size_t file) : source(source), file(file), lexer(source), token(lexer.next()), next(lexer.next()) {}

    void appendTo(Module& module) {
        while (token.kind != Kind::End) {
            auto firstPrefix = module.expansions.size();
            while (is("#") || is("@")) {
                bool decorator = text(consume()) == "@";
                auto name = identifier();
                std::optional<Identifier> argument;
                if (is("(")) { consume(); argument = identifier(); require(")"); }
                module.expansions.push_back({file, std::move(name), std::move(argument), decorator, false, 0});
            }
            bool pub = visibility();
            for (auto index = firstPrefix; index < module.expansions.size(); ++index) {
                auto& prefix = module.expansions[index];
                if (is("import")) throw at(source, prefix.name.span, "LYM0001", "不能修饰 import");
                prefix.record = is("struct") || is("class");
                prefix.target = prefix.record ? module.aggregates.size() : module.functions.size();
            }
            if (is("import")) {
                consume();
                std::vector<Identifier> path{identifier()};
                while (is(".")) { consume(); path.push_back(identifier()); }
                auto alias = path.back();
                if (is("as")) { consume(); alias = identifier(); }
                module.imports.push_back({file, pub, std::move(path), std::move(alias)});
                if (is(";")) consume();
                else if (token.kind != Kind::End && !token.newlineBefore) throw at(source, token.span, "LYP0001", "导入后需要换行或分号");
            } else if (is("struct") || is("class")) aggregate(module, pub);
            else module.functions.push_back(function({}, pub));
        }
    }

private:
    const Source& source;
    std::size_t file;
    Lexer lexer;
    Token token, next;
    std::vector<Expr> nodes;

    Token consume() {
        auto previous = token;
        token = next;
        next = lexer.next();
        return previous;
    }

    std::string_view text(Token item) const { return std::string_view(source.text).substr(item.span.begin, item.span.end - item.span.begin); }
    bool is(std::string_view word) const { return text(token) == word; }
    Token require(std::string_view word) {
        if (!is(word)) throw at(source, token.span, "LYP0001", "此处需要 " + std::string(word));
        return consume();
    }

    Identifier identifier() {
        if (token.kind != Kind::Name && token.kind != Kind::Escaped) throw at(source, token.span, "LYP0001", "此处需要标识符");
        auto item = consume();
        auto word = text(item);
        if (item.kind == Kind::Escaped) word = word.substr(1, word.size() - 2);
        return {std::string(word), item.span};
    }
    ExprId add(Span span, auto node) {
        nodes.push_back({span, std::move(node)});
        return nodes.size() - 1;
    }

    Identifier type() {
        auto name = identifier();
        if (is("<")) {
            consume();
            name.text += '<' + type().text;
            while (is(",")) { consume(); name.text += ',' + type().text; }
            name.span.end = require(">").span.end;
            name.text += '>';
        }
        while (is("?")) { name.span.end = consume().span.end; name.text = "Option<" + name.text + ">"; }
        return name;
    }

    std::vector<Identifier> typeList(bool declarations = false) {
        require("<");
        std::vector<Identifier> result{declarations ? identifier() : type()};
        while (is(",")) { consume(); result.push_back(declarations ? identifier() : type()); }
        require(">");
        return result;
    }

    bool explicitTypes(const Identifier& name) const {
        if (!is("<") || token.span.begin != name.span.end) return false;
        auto look = lexer;
        auto current = next;
        unsigned depth = 1;
        while (true) {
            auto word = text(current);
            if (word == ">" && !--depth) { current = look.next(); return text(current) == "(" && !current.newlineBefore; }
            if (word == "<") ++depth;
            else if (word != ">" && word != "," && word != "?" && current.kind != Kind::Name && current.kind != Kind::Escaped) return false;
            current = look.next();
        }
    }

    bool visibility() { if (!is("pub")) return false; consume(); return true; }

    Field field(bool pub) {
        if (!is("var") && !is("val")) throw at(source, token.span, "LYP0001", "字段声明需要 val 或 var");
        bool writable = text(consume()) == "var";
        auto name = identifier();
        require(":");
        return {std::move(name), type(), writable, pub};
    }

    void aggregate(Module& module, bool pub) {
        bool reference = text(consume()) == "class";
        Aggregate decl{file, identifier(), reference, {}, pub};
        bool primary = is("(");
        if (primary) {
            consume();
            while (!is(")")) {
                decl.fields.push_back(field(visibility()));
                if (!is(",")) break;
                consume();
            }
            require(")");
        }
        if (!primary || is("{")) {
            require("{");
            while (!is("}")) {
                bool publicMember = visibility();
                if (is("fn") || is("async") || is("extern")) module.functions.push_back(function(decl.name, publicMember));
                else decl.fields.push_back(field(publicMember));
                if (is(";")) consume();
                else if (!is("}") && !token.newlineBefore) throw at(source, token.span, "LYP0001", "成员之间需要换行或分号");
            }
            consume();
        }
        module.aggregates.push_back(std::move(decl));
    }

    Function function(std::optional<Identifier> owner, bool pub) {
        bool isAsync = false, foreign = false;
        if (is("async") || is("extern")) {
            isAsync = is("async"); foreign = !isAsync;
            consume();
            if (is("async") || is("extern")) throw at(source, token.span, "LYF0001", "声明只能使用一个 async 或 extern 修饰符");
        }
        bool conversion = is("implicit");
        auto keyword = require(conversion ? "implicit" : "fn");
        if (conversion && (owner || isAsync || foreign)) throw at(source, keyword.span, "LYV0001", "implicit 必须是顶层同步转换");
        auto name = conversion ? Identifier{"$implicit" + std::to_string(file) + "_" + std::to_string(keyword.span.begin), keyword.span} : identifier();
        auto typeParameters = is("<") ? typeList(true) : std::vector<Identifier>{};
        if (foreign && (owner || !typeParameters.empty())) throw at(source, name.span, "LYF0001", "extern 不支持方法或泛型");
        require("(");
        std::vector<Parameter> parameters;
        if (owner) {
            auto self = require("self");
            parameters.push_back({{"self", self.span}, *owner});
            if (!is(")")) require(",");
        }
        while (!is(")")) {
            if (!parameters.empty() && parameters.back().variadic) throw at(source, token.span, "LYP0001", "变长形参必须放在最后");
            if (is("...")) throw at(source, token.span, "LYP0001", "变长形参写在类型之后，例如 values: Int...");
            auto parameter = identifier();
            require(":");
            Parameter item{std::move(parameter), type()};
            if (is("...")) { consume(); item.variadic = true; }
            if (is("=")) {
                if (item.variadic) throw at(source, token.span, "LYP0001", "变长形参不能设置默认值");
                consume(); item.defaultValue = expression(true);
            }
            if (foreign && (item.variadic || item.defaultValue)) throw at(source, item.name.span, "LYF0001", "extern 形参不支持变长或默认值");
            parameters.push_back(std::move(item));
            if (!is(",")) break;
            consume();
        }
        require(")");
        if (conversion && (!typeParameters.empty() || parameters.size() != 1 || parameters.front().defaultValue || parameters.front().variadic)) throw at(source, keyword.span, "LYV0001", "implicit 需要一个无默认值的普通形参");
        require(":");
        auto result = type();
        if (foreign) {
            require(";");
            auto symbol = name;
            return {file, std::move(name), std::move(parameters), std::move(result), {}, 0, {}, pub, {}, false, std::move(symbol)};
        }
        auto body = block();
        return {file, std::move(name), std::move(parameters), std::move(result), std::exchange(nodes, {}), body, std::move(owner), pub, std::move(typeParameters), isAsync, {}, conversion};
    }

    ExprId block() {
        auto start = require("{").span.begin;
        Block body;
        while (!is("}")) {
            if (token.kind == Kind::End) throw at(source, token.span, "LYP0001", "块缺少 }");
            auto value = statement();
            std::optional<Span> semi;
            if (is(";")) semi = consume().span;
            else if (!is("}") && token.kind != Kind::End && !token.newlineBefore) {
                throw at(source, token.span, "LYP0001", "表达式之间需要换行或分号");
            }
            body.statements.push_back({value, semi});
        }
        body.end = consume().span;
        auto span = Span{start, body.end.end};
        return add(span, std::move(body));
    }

    ExprId statement() {
        auto start = token.span.begin;
        if (is("defer")) {
            consume();
            auto body = expression();
            return add({start, nodes[body].span.end}, Deferred{body});
        }
        if (is("val") || is("var")) {
            bool writable = text(consume()) == "var";
            auto name = identifier();
            std::optional<Identifier> annotation;
            if (is(":")) { consume(); annotation = type(); }
            require("=");
            auto value = expression();
            return add({start, nodes[value].span.end}, Binding{std::move(name), writable, std::move(annotation), value});
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
            auto condition = expression(false, 1, false);
            auto body = block();
            return add({start, nodes[body].span.end}, While{condition, body});
        }
        return expression();
    }

    ExprId expression(bool inParens = false, int minRank = 1, bool trailingBlock = true) {
        auto left = primary(inParens, true, trailingBlock);
        int previous = 0;
        while (inParens || !token.newlineBefore) {
            auto op = std::ranges::find(infix, text(token), &Infix::text);
            if (op == std::end(infix) || op->rank < minRank) break;
            if ((op->rank == 4 || op->rank == 1) && previous == op->rank) throw at(source, token.span, "LYP0001", "比较或赋值不能连写");
            previous = op->rank;
            auto site = consume().span;
            auto right = expression(inParens, op->rank + 1, trailingBlock);
            left = add({nodes[left].span.begin, nodes[right].span.end}, Binary{op->op, site, left, right});
        }
        return left;
    }

    ExprId primary(bool inParens, bool propagate = true, bool trailingBlock = true) {
        auto value = atom(inParens, trailingBlock);
        while (true) {
            if (is("[") && (inParens || !token.newlineBefore)) {
                auto site = consume().span;
                auto offset = expression(true);
                auto end = require("]").span.end;
                value = add({nodes[value].span.begin, end}, Index{value, offset, site});
                continue;
            }
            if (propagate && is("?") && (inParens || !token.newlineBefore)) {
                auto site = consume().span;
                value = add({nodes[value].span.begin, site.end}, Propagate{value, site});
                continue;
            }
            if (!is(".")) break;
            consume();
            auto name = identifier();
            auto begin = nodes[value].span.begin;
            if (explicitTypes(name) || (is("(") && (inParens || !token.newlineBefore))) value = call(begin, std::move(name), value);
            else { auto end = name.span.end; value = add({begin, end}, Member{value, std::move(name)}); }
        }
        return value;
    }

    SumPattern pattern() {
        SumPattern result{token.span, {}, {}};
        std::size_t open = 0;
        for (;;) {
            bool escaped = token.kind == Kind::Escaped;
            auto name = identifier();
            result.span.end = name.span.end;
            auto tag = escaped ? std::nullopt : sumTag(name.text);
            if (!tag) {
                if (name.text != "_" || escaped) result.binding = PatternBinding{std::move(name.text), name.span};
                break;
            }
            result.tags.push_back({*tag, name.span});
            if (*tag == SumTag::None) break;
            require("(");
            ++open;
        }
        while (open > 0) { --open; result.span.end = require(")").span.end; }
        return result;
    }

    ExprId call(std::size_t begin, Identifier name, std::optional<ExprId> receiver) {
        auto types = is("<") ? typeList() : std::vector<Identifier>{};
        require("(");
        Call node{std::move(name), {}, receiver, std::move(types)};
        while (!is(")")) {
            std::optional<Identifier> label;
            if (text(next) == ":") { label = identifier(); consume(); }
            std::optional<Span> spread;
            if (is("...")) spread = consume().span;
            node.arguments.push_back({std::move(label), expression(true), spread});
            if (!is(",")) break;
            consume();
        }
        auto close = require(")").span;
        return add({begin, close.end}, std::move(node));
    }

    ExprId atom(bool inParens, bool trailingBlock) {
        auto start = token.span.begin;
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
            auto item = consume();
            auto literal = text(item);
            std::string result;
            for (std::size_t offset = 1; offset + 1 < literal.size(); ++offset) {
                auto byte = literal[offset];
                if (byte == '\\') {
                    byte = literal[++offset];
                    switch (byte) { case 'n': byte = '\n'; break; case 'r': byte = '\r'; break; case 't': byte = '\t'; break; case '0': byte = '\0'; break; }
                }
                result.push_back(byte);
            }
            return add(item.span, StringLiteral{std::move(result)});
        }
        if (token.kind == Kind::Number || (is("-") && next.kind == Kind::Number)) {
            bool negative = is("-");
            if (negative) consume();
            auto digits = consume().span;
            return add({start, digits.end}, integer(source, digits, negative));
        }
        if (is("await") || is("spawn")) {
            auto op = consume();
            auto operand = primary(inParens, false, trailingBlock);
            return add({start, nodes[operand].span.end}, TaskOp{text(op) == "spawn", op.span, operand});
        }
        if (is("-") || is("!")) {
            auto item = consume();
            auto operand = primary(inParens, true, trailingBlock);
            return add({start, nodes[operand].span.end}, Unary{text(item) == "!" ? Op::Not : Op::Sub, item.span, operand});
        }
        if (is("true") || is("false")) {
            auto item = consume();
            return add(item.span, text(item) == "true");
        }
        if (is("null") || is("None")) return add(consume().span, EmptyOption{});
        if (is("match")) {
            consume();
            Match match{expression(false, 1, false), {}};
            require("{");
            while (!is("}")) {
                auto branch = pattern();
                require("=>");
                match.arms.push_back({std::move(branch), statement()});
                if (!is(",")) break;
                consume();
            }
            auto end = require("}").span.end;
            return add({start, end}, std::move(match));
        }
        if (is("if")) {
            consume();
            auto condition = expression(false, 1, false);
            auto yes = block();
            std::optional<ExprId> no;
            if (is("else")) { consume(); no = is("if") ? primary(false) : block(); }
            return add({start, nodes[no.value_or(yes)].span.end}, Conditional{condition, yes, no});
        }
        if (trailingBlock && is("blocking") && text(next) == "{" && !next.newlineBefore) {
            consume();
            auto body = block();
            return add({start, nodes[body].span.end}, Blocking{body});
        }
        if (is("{")) return block();
        if (token.kind == Kind::Name || token.kind == Kind::Escaped) {
            auto name = identifier();
            if (!explicitTypes(name) && (!is("(") || (token.newlineBefore && !inParens))) { auto span = name.span; return add(span, std::move(name)); }
            return call(start, std::move(name), {});
        }
        if (is("(")) {
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

void parseFile(Module& module, const Source& source, std::size_t file) {
    Parser(source, file).appendTo(module);
}

}
