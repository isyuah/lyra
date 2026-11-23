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

using ExprId = std::size_t;

struct Identifier { std::string text; Span span; };
struct Argument { std::optional<Identifier> label; ExprId value; };
struct Call {
    Identifier name;
    std::vector<Argument> arguments;
};

enum class Op { Set, AddSet, SubSet, MulSet, DivSet, RemSet, Or, And, Eq, Ne, Lt, Le, Gt, Ge, Add, Sub, Mul, Div, Rem, Not };
struct Unary { Op op; Span site; ExprId operand; };
struct Binary { Op op; Span site; ExprId left, right; };
struct Statement { ExprId expression; std::optional<Span> semicolon; };
struct Block { std::vector<Statement> statements; Span end; };
struct Binding { Identifier name; bool writable; std::optional<Identifier> annotation; ExprId value; };
struct Conditional { ExprId condition, yes; std::optional<ExprId> no; };
struct While { ExprId condition, body; };
struct Return { std::optional<ExprId> value; };
enum class Jump { Break, Continue };

struct Expr {
    Span span;
    std::variant<std::int64_t, bool, Identifier, Call, Unary, Binary, Block, Binding, Conditional, While, Return, Jump> node;
};

struct Parameter { Identifier name, type; };
struct Function {
    std::size_t file;
    Identifier name;
    std::vector<Parameter> parameters;
    Identifier result;
    std::vector<Expr> expressions;
    ExprId body;
};

using Module = std::vector<Function>;
Module parse(const std::vector<Source>& sources);

}
