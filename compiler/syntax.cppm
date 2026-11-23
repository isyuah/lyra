module;
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

export module lyra.syntax;
export import lyra.source;

export namespace lyra {

enum class Type { Int, Unit };
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

}
