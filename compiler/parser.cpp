#include "frontend.hpp"

#include <charconv>
#include <limits>
#include <string_view>

namespace lyra {
namespace {

enum class Kind { Fn, Name, Number, LeftParen, RightParen, LeftBrace, RightBrace, Colon, Comma, Plus, Minus, Semi, End };

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
            kind = isDigit(current) ? Kind::Number : text.substr(start, offset - start) == "fn" ? Kind::Fn : Kind::Name;
        } else {
            switch (current) {
                case '(': kind = Kind::LeftParen; break;
                case ')': kind = Kind::RightParen; break;
                case '{': kind = Kind::LeftBrace; break;
                case '}': kind = Kind::RightBrace; break;
                case ':': kind = Kind::Colon; break;
                case ',': kind = Kind::Comma; break;
                case '+': kind = Kind::Plus; break;
                case '-': kind = Kind::Minus; break;
                case ';': kind = Kind::Semi; break;
                default:
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

class Parser {
public:
    Parser(const Source& source, std::size_t file) : source(source), file(file), lexer(source), token(lexer.next()) {}

    void appendTo(Module& module) {
        while (token.kind != Kind::End) module.push_back(function());
    }

private:
    const Source& source;
    std::size_t file;
    Lexer lexer;
    Token token;

    Token consume() {
        auto previous = token;
        token = lexer.next();
        return previous;
    }

    Token require(Kind kind, const char* message) {
        if (token.kind != kind) throw at(source, token.span, "LYP0001", message);
        return consume();
    }

    std::string spelling(Span span) const { return source.text.substr(span.begin, span.end - span.begin); }

    Function function() {
        require(Kind::Fn, "期望以 fn 开始的具名函数声明");
        auto name = require(Kind::Name, "fn 后需要函数名").span;
        require(Kind::LeftParen, "函数名后需要 (");
        require(Kind::RightParen, "此切片只支持无参函数，期望 )");
        require(Kind::Colon, "此切片需要显式返回类型 : Int 或 : Unit");
        auto result = require(Kind::Name, "需要返回类型").span;
        auto type = spelling(result);
        if (type != "Int" && type != "Unit") throw at(source, result, "LYT0003", "不支持的返回类型：" + type);
        Function parsed{file, spelling(name), name, type == "Int" ? Type::Int : Type::Unit, result, {}, {}, {}};
        require(Kind::LeftBrace, "函数体需要 {");
        while (token.kind != Kind::RightBrace) {
            if (token.kind == Kind::End) throw at(source, token.span, "LYP0001", "函数体缺少 }");
            auto value = expression(parsed, false);
            std::optional<Span> semi;
            if (token.kind == Kind::Semi) semi = consume().span;
            else if (token.kind != Kind::RightBrace && token.kind != Kind::End && !token.newlineBefore) {
                throw at(source, token.span, "LYP0001", "表达式之间需要换行或分号");
            }
            parsed.body.push_back({value, semi});
        }
        parsed.closingBrace = consume().span;
        return parsed;
    }

    ExprId expression(Function& parsed, bool inParens) {
        auto left = primary(parsed, inParens);
        while (token.kind == Kind::Plus && (inParens || !token.newlineBefore)) {
            auto operation = consume().span;
            auto right = primary(parsed, inParens);
            Span span{parsed.expressions[left].span.begin, parsed.expressions[right].span.end};
            parsed.expressions.push_back({span, Add{left, right, operation}});
            left = parsed.expressions.size() - 1;
        }
        return left;
    }

    ExprId primary(Function& parsed, bool inParens) {
        auto start = token.span.begin;
        if (token.kind == Kind::Number || token.kind == Kind::Minus) {
            bool negative = token.kind == Kind::Minus;
            if (negative) consume();
            auto digits = require(Kind::Number, "此切片的负号后需要整数字面量").span;
            parsed.expressions.push_back({{start, digits.end}, integer(source, digits, negative)});
        } else if (token.kind == Kind::Name) {
            auto name = consume().span;
            if (token.newlineBefore && !inParens) throw at(source, name, "LYP0001", "此处需要函数调用");
            require(Kind::LeftParen, "此处需要函数调用，函数名后需要 (");
            Call call{spelling(name), name, {}};
            if (token.kind != Kind::RightParen) {
                for (;;) {
                    call.arguments.push_back(expression(parsed, true));
                    if (token.kind != Kind::Comma) break;
                    consume();
                }
            }
            auto close = require(Kind::RightParen, "调用缺少 )").span;
            parsed.expressions.push_back({{start, close.end}, std::move(call)});
        } else if (token.kind == Kind::LeftParen) {
            consume();
            auto value = expression(parsed, true);
            auto close = require(Kind::RightParen, "分组表达式缺少 )").span;
            parsed.expressions[value].span = {start, close.end};
            return value;
        } else throw at(source, token.span, "LYP0001", "期望整数、函数调用或括号表达式");
        return parsed.expressions.size() - 1;
    }
};

}

Module parse(const std::vector<Source>& sources) {
    Module functions;
    for (std::size_t index = 0; index < sources.size(); ++index) Parser(sources[index], index).appendTo(functions);
    return functions;
}

}
