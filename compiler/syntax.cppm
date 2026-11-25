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

struct Identifier { std::string text; Span span; std::string linked{}; bool exportedOnly{}; std::optional<SourceAnchor> renameTarget{}; };
struct Attribute { Identifier name; std::optional<Identifier> argument; };
struct Argument { std::optional<Identifier> label; ExprId value; std::optional<Span> spread{}, trailing{}; };
struct TypeQualifier { Identifier name; std::vector<Identifier> arguments{}; };
struct Call {
    Identifier name;
    std::vector<Argument> arguments;
    std::optional<ExprId> receiver{};
    std::vector<Identifier> typeArguments{};
    std::optional<ExprId> function{};
    std::optional<TypeQualifier> typeOwner{};
    std::optional<std::string> boundTrait{};
};
struct Member { ExprId receiver; Identifier name; };
struct QualifiedMember { TypeQualifier owner; Identifier name; };
struct This {};
struct ListLiteral { std::vector<ExprId> elements; };
struct Index { ExprId receiver, offset; Span site; };
struct StringLiteral { std::string bytes; };
struct BytesLiteral { std::string bytes; };
struct CharLiteral { std::uint32_t point; };
struct DisplayValue { ExprId value; };
struct TextTemplate { std::vector<ExprId> parts; };
struct IntegerLiteral { std::uint64_t magnitude{}; bool negative{}; };
struct RealLiteral { std::string digits; std::optional<unsigned> width; };

enum class Op { Set, AddSet, SubSet, MulSet, DivSet, RemSet, Or, And, Eq, Ne, Lt, Le, Gt, Ge, Add, Sub, Mul, Div, Rem, Not };
struct Unary { Op op; Span site; ExprId operand; };
struct Binary { Op op; Span site; ExprId left, right; };
struct RangeValue { ExprId start, end; bool inclusive; };
enum class TypeOperator { Is, CheckedCast, InterfaceCast };
struct TypeOperation { TypeOperator op; ExprId value; Identifier type; Span site; };
struct Statement { ExprId expression; std::optional<Span> semicolon; };
struct Block { std::vector<Statement> statements; Span end; };
struct Binding { Identifier name; bool writable; std::optional<Identifier> annotation; ExprId value; };
struct Conditional { ExprId condition, yes; std::optional<ExprId> no; };
struct While { ExprId condition, body; };
struct For { Identifier item; ExprId input, body; };
struct Return { std::optional<ExprId> value; };
struct Raise { Span site; std::optional<ExprId> value; };
struct CatchBinding { Identifier name, type; };
struct CatchArm { Span site; std::optional<CatchBinding> binding; std::optional<ExprId> guard; ExprId body; };
struct Attempt { ExprId body; std::vector<CatchArm> arms; };
struct TaskOp { bool spawn; Span site; ExprId operand; };
struct Blocking { ExprId body; };
struct AsyncBlock { enum Kind { Group, Parallel }; Kind kind; ExprId body; Span site; };
struct Deferred { ExprId body; };
struct LambdaParameter { Identifier name; std::optional<Identifier> type; };
struct Lambda { std::vector<LambdaParameter> parameters; std::optional<Identifier> result; ExprId body; bool implicit; };
struct EmptyOption {};
struct Propagate { ExprId operand; Span site; };
struct PatternName {
    Identifier name;
    std::optional<Identifier> owner{};
    std::optional<std::vector<std::optional<Identifier>>> labels{};
    bool escaped{};
};
struct PatternIntegerRange { IntegerLiteral lower, upper; bool inclusive; };
struct PatternAlternatives {};
struct PatternList { bool openEnd; std::optional<Identifier> rest; };
struct Pattern {
    Span span;
    std::variant<std::monostate, PatternName, SumTag, IntegerLiteral, PatternIntegerRange, PatternAlternatives, PatternList, bool, StringLiteral, CharLiteral> node;
    std::vector<Pattern> parts{};
};
struct PatternBinding { Pattern pattern; std::optional<Identifier> annotation; ExprId value; };
struct MatchArm { Pattern pattern; ExprId value; std::optional<ExprId> guard{}; };
enum class OptionalForm { None, Access, Default };
struct Match { ExprId subject; std::vector<MatchArm> arms; OptionalForm optional{}; Span operation{}; };
enum class Jump { Break, Continue };

struct Expr {
    Span span;
    std::variant<IntegerLiteral, RealLiteral, bool, StringLiteral, BytesLiteral, CharLiteral, DisplayValue, TextTemplate, Identifier, This, Call, Member, QualifiedMember, Unary, Binary, RangeValue, TypeOperation, Block, Binding, PatternBinding, Conditional, While, For, Return, Raise, Attempt, Jump, TaskOp, Blocking, AsyncBlock, Deferred, EmptyOption, Propagate, Match, ListLiteral, Index, Lambda> node;
};

struct Parameter { Identifier name, type; bool variadic{}; std::optional<ExprId> defaultValue{}; std::vector<Attribute> attributes{}; };
struct Bound { std::size_t parameter; Identifier trait; };
struct ExtensionProperty { Identifier name, type; std::size_t getter; };
struct Extension { std::size_t file; Identifier name, receiver; bool isPublic; std::vector<Identifier> typeParameters{}; std::vector<Bound> bounds{}; std::vector<ExtensionProperty> properties{}; };
struct TraitMethod { Identifier name; std::vector<Parameter> parameters; Identifier result; bool asynchronous; };
struct Trait { std::size_t file; Identifier name; std::vector<TraitMethod> methods; bool isPublic; std::vector<Identifier> associated{}; };
enum class ConstKind { None, Value, Function };
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
    std::vector<Bound> bounds{};
    bool initializer{};
    bool hasReceiver{};
    std::optional<Identifier> extensionGroup{};
    ConstKind constant{};
    bool extensionProperty{};
    std::vector<Identifier> throwsTypes{};
};

struct FieldDefault { std::size_t function; bool emptyOption; };
struct Field { Identifier name, type; bool writable; bool isPublic{}; std::vector<Attribute> attributes{}; std::optional<FieldDefault> defaultValue{}; };
std::string fieldDefaultName(const std::string& record, const std::string& field) { return record + "$fieldDefault$" + field; }
struct TypeMember { Identifier name, type; bool isPublic; };
struct Aggregate { std::size_t file; Identifier name; bool reference; std::vector<Field> fields; bool isPublic{}; std::vector<Identifier> typeParameters{}; std::vector<Bound> bounds{}; Span span{}; std::vector<TypeMember> associated{}; };
struct EnumField { std::optional<Identifier> name; Identifier type; };
struct EnumCase { Identifier name; std::vector<EnumField> fields; };
struct Enumeration { std::size_t file; Identifier name; std::vector<EnumCase> cases; bool isPublic; std::vector<Identifier> typeParameters{}; std::vector<Bound> bounds{}; };
struct ExceptionType { std::size_t file; Identifier name; bool isPublic; };
struct TypeAlias { std::size_t file; Identifier name, target; bool isPublic; };
struct Import { std::size_t file; bool isPublic; std::vector<Identifier> path; Identifier alias; };
struct Expansion { std::size_t file; Identifier name; std::optional<Identifier> argument; bool decorator; bool record; std::size_t target; std::vector<Identifier> exports{}; };
struct MacroArgument { Span span; std::optional<std::string> literal; };
struct ExpressionExpansion { Identifier name; Span span, argumentsSpan; std::vector<MacroArgument> arguments; };
struct SyntaxToken { Span span; std::string kind; };
struct Module { std::vector<Function> functions; std::vector<Aggregate> aggregates; std::vector<Import> imports; std::vector<Expansion> expansions; std::vector<Trait> traits{}; std::vector<Enumeration> enums{}; std::vector<Extension> extensions{}; std::vector<std::vector<std::string>> importedExtensions{}; std::vector<ExceptionType> exceptions{}; std::vector<TypeAlias> aliases{}; };
Module parse(const std::vector<Source>& sources);
void parseFile(Module& module, const Source& source, std::size_t file, std::vector<ExpressionExpansion>* expressions = nullptr);
void validateMacroExpression(const Source& generated);
std::vector<SyntaxToken> syntaxTokens(const Source& source, bool comments = false, bool partial = false);

}
