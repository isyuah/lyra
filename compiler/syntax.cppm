module;
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

export module lyra.syntax;
export import lyra.source;
export import lyra.sums;

export namespace lyra {

using ExprId = std::size_t;

struct Identifier { std::string text; Span span; };
struct Attribute { Identifier name; std::optional<Identifier> argument; };
struct Argument { std::optional<Identifier> label; ExprId value; std::optional<Span> spread{}; };
struct Call {
    Identifier name;
    std::vector<Argument> arguments;
    std::optional<ExprId> receiver{};
    std::vector<Identifier> typeArguments{};
};
struct Member { ExprId receiver; Identifier name; bool iterationExtent{}; };
struct ListLiteral { std::vector<ExprId> elements; };
struct Index { ExprId receiver, offset; Span site; };
struct StringLiteral { std::string bytes; };

enum class Op { Set, AddSet, SubSet, MulSet, DivSet, RemSet, Or, And, Eq, Ne, Lt, Le, Gt, Ge, Add, Sub, Mul, Div, Rem, Not };
struct Unary { Op op; Span site; ExprId operand; };
struct Binary { Op op; Span site; ExprId left, right; };
struct Statement { ExprId expression; std::optional<Span> semicolon; };
struct Block { std::vector<Statement> statements; Span end; };
struct Binding { Identifier name; bool writable; std::optional<Identifier> annotation; ExprId value; };
struct Conditional { ExprId condition, yes; std::optional<ExprId> no; };
struct While { ExprId condition, body; };
struct Return { std::optional<ExprId> value; };
struct Raise { Span site; std::optional<ExprId> value; };
struct CatchBinding { Identifier name, type; };
struct Attempt { ExprId body, handler; std::optional<CatchBinding> binding; };
struct TaskOp { bool spawn; Span site; ExprId operand; };
struct Blocking { ExprId body; };
struct Deferred { ExprId body; };
struct EmptyOption {};
struct Propagate { ExprId operand; Span site; };
struct MatchArm { SumPattern pattern; ExprId value; };
enum class OptionalForm { None, Access, Default };
struct Match { ExprId subject; std::vector<MatchArm> arms; OptionalForm optional{}; Span operation{}; };
enum class Jump { Break, Continue };

struct Expr {
    Span span;
    std::variant<std::int64_t, bool, StringLiteral, Identifier, Call, Member, Unary, Binary, Block, Binding, Conditional, While, Return, Raise, Attempt, Jump, TaskOp, Blocking, Deferred, EmptyOption, Propagate, Match, ListLiteral, Index> node;
};

struct Parameter { Identifier name, type; bool variadic{}; std::optional<ExprId> defaultValue{}; std::vector<Attribute> attributes{}; };
struct Function {
    std::size_t file;
    Identifier name;
    std::vector<Parameter> parameters;
    Identifier result;
    std::vector<Expr> expressions;
    ExprId body;
    std::optional<Identifier> owner{};
    bool isPublic{};
    std::vector<Identifier> typeParameters{};
    bool isAsync{};
    std::optional<Identifier> foreignSymbol{};
    bool conversion{};
};

struct Field { Identifier name, type; bool writable; bool isPublic{}; std::vector<Attribute> attributes{}; };
struct Aggregate { std::size_t file; Identifier name; bool reference; std::vector<Field> fields; bool isPublic{}; };
struct Import { std::size_t file; bool isPublic; std::vector<Identifier> path; Identifier alias; };
struct Expansion { std::size_t file; Identifier name; std::optional<Identifier> argument; bool decorator; bool record; std::size_t target; };
struct MacroArgument { Span span; std::optional<std::string> literal; };
struct ExpressionExpansion { Identifier name; Span span, argumentsSpan; std::vector<MacroArgument> arguments; };
struct MacroSplice { Span span; std::size_t argument; };
struct Module { std::vector<Function> functions; std::vector<Aggregate> aggregates; std::vector<Import> imports; std::vector<Expansion> expansions; };
Module parse(const std::vector<Source>& sources);
void parseFile(Module& module, const Source& source, std::size_t file, std::vector<ExpressionExpansion>* expressions = nullptr);
std::vector<MacroSplice> macroSplices(const Source& generated, std::size_t count);

}
