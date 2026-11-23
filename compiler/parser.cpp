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
            constexpr std::string_view keywords[] = {"fn", "val", "var", "if", "else", "while", "for", "in", "return", "break", "continue", "true", "false", "struct", "class", "pub", "import", "as", "async", "await", "spawn", "extern", "match", "null", "implicit", "defer", "throw", "try", "catch"};
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
            constexpr std::string_view doubles[] = {"+=", "-=", "*=", "/=", "%=", "==", "!=", "<=", ">=", "&&", "||", "=>", "?.", "??", "->"};
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
    {"||", 3, Op::Or}, {"&&", 4, Op::And}, {"==", 5, Op::Eq}, {"!=", 5, Op::Ne}, {"<", 5, Op::Lt}, {"<=", 5, Op::Le}, {">", 5, Op::Gt}, {">=", 5, Op::Ge},
    {"+", 6, Op::Add}, {"-", 6, Op::Sub}, {"*", 7, Op::Mul}, {"/", 7, Op::Div}, {"%", 7, Op::Rem}
};

class Parser {
public:
    Parser(const Source& source, std::size_t file, std::vector<ExpressionExpansion>* expressions = nullptr) : source(source), file(file), lexer(source), token(lexer.next()), next(lexer.next()), macros(expressions) {}

    void appendTo(Module& module) {
        while (token.kind != Kind::End) {
            auto firstPrefix = module.expansions.size();
            while (is("#") || is("@")) {
                bool decorator = text(consume()) == "@";
                auto name = identifier();
                std::optional<Identifier> argument;
                if (is("(")) { consume(); argument = metadataValue(); require(")"); }
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
    std::vector<ExpressionExpansion>* macros;
    std::vector<Expr> nodes;

    struct Lookahead {};
    Parser(const Parser& parser, Lookahead) : source(parser.source), file(parser.file), lexer(parser.lexer), token(parser.token), next(parser.next), macros(nullptr) {}

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
    Identifier stringLiteral() {
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
        return {std::move(result), item.span};
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
            while (is("?")) { name.span.end = consume().span.end; name.text = "Option<" + name.text + ">"; }
            return name;
        }
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

    bool explicitTypes(const Identifier& name) const { return explicitTypesAfter(name.span.end); }

    bool explicitTypesAfter(std::size_t nameEnd) const {
        if (!is("<") || token.span.begin != nameEnd) return false;
        Parser look(*this, Lookahead{});
        try { look.typeList(); }
        catch (const Diagnostic&) { return false; }
        return look.is("(") && !look.token.newlineBefore;
    }

    bool visibility() { if (!is("pub")) return false; consume(); return true; }

    Field field(bool pub, std::vector<Attribute> marks) {
        if (!is("var") && !is("val")) throw at(source, token.span, "LYP0001", "字段声明需要 val 或 var");
        bool writable = text(consume()) == "var";
        auto name = identifier();
        require(":");
        return {std::move(name), type(), writable, pub, std::move(marks)};
    }

    void aggregate(Module& module, bool pub) {
        bool reference = text(consume()) == "class";
        Aggregate decl{file, identifier(), reference, {}, pub};
        bool primary = is("(");
        if (primary) {
            consume();
            while (!is(")")) {
                auto marks = attributes();
                decl.fields.push_back(field(visibility(), std::move(marks)));
                if (!is(",")) break;
                consume();
            }
            require(")");
        }
        if (!primary || is("{")) {
            require("{");
            while (!is("}")) {
                auto marks = attributes();
                bool publicMember = visibility();
                if (is("fn") || is("async") || is("extern")) {
                    if (!marks.empty()) throw at(source, marks.front().name.span, "LYM0003", "此处的标记只适用于字段和形参");
                    module.functions.push_back(function(decl.name, publicMember));
                } else decl.fields.push_back(field(publicMember, std::move(marks)));
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
            if (!parameters.empty() && parameters.back().variadic) throw at(source, token.span, "LYP0001", "变长形参 " + parameters.back().name.text + " 必须放在最后");
            if (is("...")) throw at(source, token.span, "LYP0001", "变长形参写在类型之后，例如 values: Int...");
            auto marks = attributes();
            auto parameter = identifier();
            require(":");
            Parameter item{std::move(parameter), type()};
            item.attributes = std::move(marks);
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
        if (is("for")) return forLoop();
        return expression();
    }

    Identifier hidden(std::string_view role, Span site) {
        return {"$" + std::string(role) + std::to_string(site.begin), site};
    }

    ExprId forLoop() {
        auto site = consume().span;
        auto item = identifier();
        require("in");
        auto source = expression(false, 1, false);
        auto body = block();
        auto span = Span{site.begin, nodes[body].span.end};
        auto input = nodes[source].span;
        auto list = hidden("list", input), count = hidden("count", site), index = hidden("index", site);
        auto reference = [&](const Identifier& name) { return add(name.span, name); };
        auto stored = add(input, Binding{list, false, {}, source});
        auto length = add(input, Member{reference(list), {"size", input}, true});
        auto extent = add(input, Binding{count, false, {}, length});
        auto counter = add(site, Binding{index, true, {}, add(site, std::int64_t{0})});
        auto test = add(site, Binary{Op::Lt, site, reference(index), reference(count)});
        auto value = add(input, Index{reference(list), reference(index), input});
        auto element = add(item.span, Binding{item, false, {}, value});
        auto advance = add(site, Binary{Op::AddSet, site, reference(index), add(site, std::int64_t{1})});
        auto step = add(span, Block{{{element, {}}, {advance, {}}, {body, {}}}, nodes[body].span});
        auto loop = add(span, While{test, step});
        return add(span, Block{{{stored, {}}, {extent, {}}, {counter, {}}, {loop, {}}}, nodes[body].span});
    }

    ExprId optional(ExprId subject, Identifier payload, ExprId present, ExprId absent, Span site, Span span, OptionalForm form) {
        SumPattern some{site, {{SumTag::Some, site}}, PatternBinding{std::move(payload.text), payload.span}};
        SumPattern none{site, {{SumTag::None, site}}, {}};
        return add(span, Match{subject, {{std::move(some), present}, {std::move(none), absent}}, form, site});
    }

    ExprId expression(bool inParens = false, int minRank = 1, bool trailingBlock = true) {
        auto left = primary(inParens, true, trailingBlock);
        int previous = 0;
        while (inParens || !token.newlineBefore) {
            if (is("??") && minRank <= 2) {
                auto site = consume().span;
                auto right = expression(inParens, 2, trailingBlock);
                auto name = hidden("some", site);
                auto result = add(site, name);
                left = optional(left, name, result, right, site, {nodes[left].span.begin, nodes[right].span.end}, OptionalForm::Default);
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

    ExprId primary(bool inParens, bool propagate = true, bool trailingBlock = true) {
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
            if (propagate && is("?") && (inParens || !token.newlineBefore)) {
                auto site = consume().span;
                value = add({nodes[value].span.begin, site.end}, Propagate{value, site});
                continue;
            }
            if (!is(".") && !is("?.")) break;
            bool safe = is("?.");
            auto site = consume().span;
            auto name = identifier();
            auto begin = nodes[value].span.begin;
            auto subject = value;
            std::optional<Identifier> payload;
            if (safe) { payload = hidden("some", site); value = add(site, *payload); }
            if (explicitTypes(name) || (is("(") && (inParens || !token.newlineBefore))) value = call(begin, std::move(name), value);
            else { auto end = name.span.end; value = add({begin, end}, Member{value, std::move(name)}); }
            value = tailArgument(value, trailingBlock);
            if (safe) {
                auto span = nodes[value].span;
                value = optional(subject, std::move(*payload), value, add(site, EmptyOption{}), site, span, OptionalForm::Access);
            }
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

    ExprId tailArgument(ExprId callee, bool allowed) {
        if (!allowed || !is("{") || token.newlineBefore) return callee;
        auto begin = nodes[callee].span.begin;
        if (auto name = std::get_if<Identifier>(&nodes[callee].node)) callee = add(nodes[callee].span, Call{*name, {}});
        else if (auto member = std::get_if<Member>(&nodes[callee].node)) callee = add(nodes[callee].span, Call{member->name, {}, member->receiver});
        else if (!std::holds_alternative<Call>(nodes[callee].node)) return callee;
        auto brace = token.span;
        auto body = block();
        auto value = add(nodes[body].span, Lambda{{}, {}, body, true});
        std::get<Call>(nodes[callee].node).arguments.push_back({{}, value, {}, brace});
        nodes[callee].span = {begin, nodes[body].span.end};
        return callee;
    }

    bool lambdaHead() const {
        if (next.kind != Kind::Name && next.kind != Kind::Escaped && text(next) != ")") return false;
        auto look = lexer;
        auto current = next;
        unsigned depth = 1;
        while (current.kind != Kind::End) {
            if (text(current) == "(") ++depth;
            if (text(current) == ")" && !--depth) {
                auto after = text(look.next());
                return after == "=>" || after == ":";
            }
            current = look.next();
        }
        return false;
    }

    ExprId lambda() {
        auto start = require("(").span.begin;
        Lambda literal{{}, {}, 0, false};
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
        literal.body = block();
        auto end = nodes[literal.body].span.end;
        return add({start, end}, std::move(literal));
    }

    ExprId call(std::size_t begin, Identifier name, std::optional<ExprId> receiver, std::optional<ExprId> function = {}) {
        auto types = is("<") ? typeList() : std::vector<Identifier>{};
        require("(");
        Call node{std::move(name), {}, receiver, std::move(types), function};
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

    ExprId macroExpression() {
        auto begin = require("#").span.begin;
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
                    if (is("<") && ((!delimiters.empty() && delimiters.back() == '>') || (followsName && explicitTypesAfter(end)))) delimiters.push_back('>');
                    else if (is(">") && !delimiters.empty() && delimiters.back() == '>') delimiters.pop_back();
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
        macros->push_back(std::move(expansion));
        return add({begin, close.end}, std::int64_t{0});
    }

    ExprId atom(bool inParens, bool trailingBlock) {
        auto start = token.span.begin;
        if (is("#")) return macroExpression();
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
        if (is("throw")) {
            auto site = consume().span;
            if (token.newlineBefore || is(";") || is("}") || is(")") || is(",")) return add(site, Raise{site, {}});
            auto value = expression(inParens, 1, trailingBlock);
            return add({start, nodes[value].span.end}, Raise{site, value});
        }
        if (is("try")) {
            consume();
            auto body = block();
            require("catch");
            std::optional<CatchBinding> binding;
            if (!is("{")) {
                auto name = identifier();
                require(":");
                binding = CatchBinding{std::move(name), type()};
            }
            if (is("if")) throw at(source, token.span, "LYE0002", "当前尚不支持 catch 过滤条件");
            auto handler = block();
            if (is("catch")) throw at(source, token.span, "LYE0002", "Exception 兜底捕获之后不能再写 catch 分支");
            return add({start, nodes[handler].span.end}, Attempt{body, handler, std::move(binding)});
        }
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

void parseFile(Module& module, const Source& source, std::size_t file, std::vector<ExpressionExpansion>* expressions) {
    Parser(source, file, expressions).appendTo(module);
}

std::vector<MacroSplice> macroSplices(const Source& generated, std::size_t count) {
    std::vector<MacroSplice> result;
    Lexer lexer(generated);
    constexpr std::string_view prefix = "__lyraMacroArg";
    std::vector<char> delimiters;
    for (auto token = lexer.next(); token.kind != Kind::End; token = lexer.next()) {
        auto word = std::string_view(generated.text).substr(token.span.begin, token.span.end - token.span.begin);
        if (word == "#") throw at(generated, token.span, "LYM0002", "宏不能递归生成新的宏调用");
        if (word == "(" || word == "[" || word == "{") delimiters.push_back(word == "(" ? ')' : word == "[" ? ']' : '}');
        if (word == ")" || word == "]" || word == "}") {
            if (delimiters.empty() || delimiters.back() != word.front()) throw at(generated, token.span, "LYM0002", "表达式宏输出括号不匹配");
            delimiters.pop_back();
        }
        if (token.kind != Kind::Name || !word.starts_with(prefix)) continue;
        word.remove_prefix(prefix.size());
        std::size_t index;
        auto [stopped, error] = std::from_chars(word.data(), word.data() + word.size(), index);
        if (error != std::errc{} || stopped != word.data() + word.size() || index >= count) throw at(generated, token.span, "LYM0002", "无效的宏实参插槽");
        result.push_back({token.span, index});
    }
    if (!delimiters.empty()) throw at(generated, {0, generated.text.size()}, "LYM0002", "表达式宏输出括号未闭合");
    return result;
}

}
