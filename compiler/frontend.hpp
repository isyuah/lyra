#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lyra {

struct Span {
    std::size_t begin, end;
};

struct Position {
    std::size_t offset, line, column;
};

struct Source {
    std::filesystem::path path;
    std::string text;
    std::vector<std::size_t> lineStarts;

    Position position(std::size_t offset) const;
};

struct Site {
    std::string file;
    Position start, end;
    std::string lineText;
};

struct Diagnostic {
    std::string code, message;
    std::optional<Site> site = {};
    std::string expected = {}, actual = {}, hint = {};
};

Diagnostic at(const Source& source, Span span, std::string code, std::string message);
void report(const Diagnostic& error, bool json);
std::vector<Source> loadSources(const std::filesystem::path& package);

enum class Type { Int, Unit };
const char* typeName(Type type);

using ExprId = std::size_t;

struct Call {
    std::string name;
    Span nameSpan;
    std::vector<ExprId> arguments;
};

struct Add {
    ExprId left, right;
    Span operatorSpan;
};

struct Expr {
    Span span;
    std::variant<std::int64_t, Call, Add> node;
};

struct Statement {
    ExprId expression;
    std::optional<Span> semicolon;
};

struct Function {
    std::size_t file;
    std::string name;
    Span nameSpan;
    Type result;
    Span resultSpan, closingBrace;
    std::vector<Expr> expressions;
    std::vector<Statement> body;
};

using Module = std::vector<Function>;

Module parse(const std::vector<Source>& sources);
void check(const Module& functions, const std::vector<Source>& sources);

}
