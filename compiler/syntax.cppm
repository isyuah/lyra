module;
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module lyra.syntax;
export import lyra.source;
export import lyra.sums;

export namespace lyra {

using ExprId = std::size_t;

struct Identifier {
    std::string text;
    Span span;
    std::string linked{};
    bool exportedOnly{};
    std::optional<SourceAnchor> renameTarget{};
    std::optional<SourceAnchor> moduleAlias{};
    bool moduleMember{};
    bool internalReference{};
};
struct Attribute { Identifier name; std::optional<Identifier> argument; };
enum class Passing { Value, In, Ref };
constexpr std::string_view passingName(Passing passing) {
    return passing == Passing::In ? "in" : passing == Passing::Ref ? "ref" : "";
}
struct Argument { std::optional<Identifier> label; ExprId value; std::optional<Span> spread{}, trailing{}; Passing passing{}; };
struct TypeQualifier { Identifier name; std::vector<Identifier> arguments{}; std::optional<ExprId> moduleReceiver{}; };
struct Call {
    Identifier name;
    std::vector<Argument> arguments;
    std::optional<ExprId> receiver{};
    std::vector<Identifier> typeArguments{};
    std::optional<ExprId> function{};
    std::optional<TypeQualifier> typeOwner{};
    std::optional<std::string> boundTrait{};
    std::vector<std::string> alternateTraits{};
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

enum class Op { Set, AddSet, SubSet, MulSet, DivSet, RemSet, BitAndSet, BitXorSet, BitOrSet, ShlSet, ShrSet,
    Or, And, Eq, Ne, Lt, Le, Gt, Ge, Add, Sub, Mul, Div, Rem, BitAnd, BitXor, BitOr, Shl, Shr, Not, Complement };
constexpr bool assignment(Op op) { return op <= Op::ShrSet; }
constexpr bool bitwise(Op op) { return op >= Op::BitAnd && op <= Op::Shr; }
constexpr bool shift(Op op) { return op == Op::Shl || op == Op::Shr; }
constexpr Op compoundOperator(Op op) {
    return static_cast<Op>(static_cast<int>(Op::Add) + static_cast<int>(op) - static_cast<int>(Op::AddSet));
}
struct Unary { Op op; Span site; ExprId operand; };
struct Binary { Op op; Span site; ExprId left, right; };
struct RangeValue { ExprId start, end; bool inclusive; };
enum class TypeOperator { Is, CheckedCast, InterfaceCast };
struct TypeOperation { TypeOperator op; ExprId value; Identifier type; Span site; };
struct Statement { ExprId expression; std::optional<Span> semicolon; };
struct Block { std::vector<Statement> statements; Span end; };
struct Binding { Identifier name; bool writable; std::optional<Identifier> annotation; ExprId value; bool constant{}; };
struct Conditional { ExprId condition, yes; std::optional<ExprId> no; };
struct Return { std::optional<ExprId> value; };
struct Raise { Span site; std::optional<ExprId> value; };
struct CatchBinding { Identifier name, type; };
struct CatchArm { Span site; std::optional<CatchBinding> binding; std::optional<ExprId> guard; ExprId body; };
struct Attempt { ExprId body; std::vector<CatchArm> arms; };
struct TaskOp { bool spawn; Span site; ExprId operand; };
struct Blocking { ExprId body; };
struct UnsafeBlock { ExprId body; };
struct AsyncBlock { enum Kind { Cold, Group, Parallel }; Kind kind; ExprId body; Span site; };
struct Deferred { ExprId body; };
struct LambdaParameter { Identifier name; std::optional<Identifier> type; };
struct Lambda { std::vector<LambdaParameter> parameters; std::optional<Identifier> result; ExprId body; bool implicit; bool isAsync{}; };
struct EmptyOption {};
struct Propagate { ExprId operand; Span site; };
struct PatternName {
    Identifier name;
    std::optional<Identifier> owner{};
    std::optional<std::vector<std::optional<Identifier>>> labels{};
    bool escaped{};
    std::optional<Identifier> module{};
};
struct PatternIntegerRange { IntegerLiteral lower, upper; bool inclusive; };
struct PatternAlternatives {};
struct PatternList { bool openEnd; std::optional<Identifier> rest; };
struct Pattern {
    Span span;
    std::variant<std::monostate, PatternName, SumTag, IntegerLiteral, PatternIntegerRange, PatternAlternatives, PatternList, bool, StringLiteral, CharLiteral> node;
    std::vector<Pattern> parts{};
};
struct While { ExprId condition, body; std::optional<Pattern> pattern{}; };
struct For { Pattern item; ExprId input, body; std::optional<Span> awaitSite{}; };
struct PatternBinding { Pattern pattern; std::optional<Identifier> annotation; ExprId value; };
struct MatchArm { Pattern pattern; ExprId value; std::optional<ExprId> guard{}; };
enum class OptionalForm { None, Access, Default };
struct Match { ExprId subject; std::vector<MatchArm> arms; OptionalForm optional{}; Span operation{}; };
struct SelectArm { std::optional<Identifier> binding; ExprId operation, value; };
struct Select { Span site; std::vector<SelectArm> arms; };
enum class Jump { Break, Continue };

struct Expr {
    Span span;
    std::variant<IntegerLiteral, RealLiteral, bool, StringLiteral, BytesLiteral, CharLiteral, DisplayValue, TextTemplate, Identifier, This, Call, Member, QualifiedMember, Unary, Binary, RangeValue, TypeOperation, Block, Binding, PatternBinding, Conditional, While, For, Return, Raise, Attempt, Jump, TaskOp, Blocking, UnsafeBlock, AsyncBlock, Deferred, EmptyOption, Propagate, Match, Select, ListLiteral, Index, Lambda> node;
};

struct Parameter { Identifier name, type; bool variadic{}; std::optional<ExprId> defaultValue{}; std::vector<Attribute> attributes{}; Passing passing{}; };
struct AssociatedBinding { Identifier member, type; };
struct Bound { std::size_t parameter; Identifier trait; std::vector<AssociatedBinding> bindings{}; };
struct ExtensionProperty { Identifier name, type; std::size_t getter; };
struct Extension { std::size_t file; Identifier name, receiver; bool isPublic; std::vector<Identifier> typeParameters{}; std::vector<Bound> bounds{}; std::vector<ExtensionProperty> properties{}; Span span{}; };
struct TraitMethod {
    Identifier name;
    std::vector<Parameter> parameters;
    Identifier result;
    bool asynchronous;
    Span span{};
    std::vector<Identifier> typeParameters{};
    std::vector<Bound> bounds{};
    std::optional<Identifier> defaultFunction{};
};
struct Trait { std::size_t file; Identifier name; std::vector<TraitMethod> methods; bool isPublic; std::vector<Identifier> associated{}; Span span{}; };
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
    bool property{};
    std::vector<Identifier> throwsTypes{};
    std::optional<Span> pendingExpansion{};
    Span header{};
    bool incompleteBody{};
    std::optional<Identifier> traitOwner{};
};

struct FieldDefault { std::size_t function; bool emptyOption; };
struct Field { Identifier name, type; bool writable; bool isPublic{}; std::vector<Attribute> attributes{}; std::optional<FieldDefault> defaultValue{}; Span span{}; };
std::string fieldDefaultName(const std::string& record, const std::string& field) { return record + "$fieldDefault$" + field; }
struct TypeMember { Identifier name, type; bool isPublic; };
struct Aggregate { std::size_t file; Identifier name; bool reference; std::vector<Field> fields; bool isPublic{}; std::vector<Identifier> typeParameters{}; std::vector<Bound> bounds{}; Span span{}; std::vector<TypeMember> associated{}; };
struct EnumField { std::optional<Identifier> name; Identifier type; };
struct EnumCase { Identifier name; std::vector<EnumField> fields; Span span{}; };
struct Enumeration { std::size_t file; Identifier name; std::vector<EnumCase> cases; bool isPublic; std::vector<Identifier> typeParameters{}; std::vector<Bound> bounds{}; Span span{}; };
struct ExceptionType { std::size_t file; Identifier name; bool isPublic; Span span{}; };
struct TypeAlias { std::size_t file; Identifier name, target; bool isPublic; std::vector<Identifier> typeParameters{}; Span span{}; };
Identifier nominalAnnotation(const auto& declaration) {
    auto result = declaration.name;
    if (declaration.typeParameters.empty()) return result;
    result.text += '<';
    for (const auto& parameter : declaration.typeParameters) {
        if (!result.text.ends_with('<')) result.text += ',';
        result.text += parameter.text;
    }
    result.text += '>';
    return result;
}
enum class ImportKind { Runtime, Macro, Decorator };
struct Import {
    std::size_t file;
    bool isPublic;
    std::vector<Identifier> path;
    Identifier alias;
    ImportKind kind{};
    std::optional<Identifier> member{};
};
enum class ExpansionTarget { Function, Aggregate, Enumeration };
struct Expansion { std::size_t file; Identifier name; std::optional<Identifier> argument; bool decorator; ExpansionTarget kind; std::size_t target; std::vector<Identifier> exports{}; };
struct MacroArgument { Span span; std::optional<std::string> literal; };
struct ExpressionExpansion { Identifier name; Span span, argumentsSpan; std::vector<MacroArgument> arguments; };
struct SyntaxRef { std::size_t index; };
struct SyntaxToken { Span span; std::string kind; };
enum class EditorKind { Keyword, Function, Method, Constructor, Variable, Parameter, Field, Property, Constant, Class, Struct,
    Enum, EnumCase, Trait, TypeAlias, TypeParameter, Module, Macro, Decorator, Extension };
struct NameReference { SourceAnchor use, declaration; std::optional<SourceAnchor> renameTarget{}; std::optional<EditorKind> kind{}; };
struct EditorSymbol { std::string label; EditorKind kind; Span name{}; std::string detail{}, documentation{}, insertion{}; };
struct OutlineSymbol { std::string name; EditorKind kind; Span selection, extent; std::vector<OutlineSymbol> children{}; };
struct EditorParameter { std::string name, label; bool variadic{}; };
struct EditorSignature { std::string label, documentation; std::vector<EditorParameter> parameters; std::optional<std::size_t> active{}; };
enum class CompletionPlace { None, TopLevel, MemberDeclaration, FieldDeclaration, TraitDeclaration, ExtensionDeclaration, EnumMethod, FunctionModifier,
    Statement, Expression, AfterExpression, Type, Constraint, Import, ImportMember, ImportAlias, Member, TypeMember, ConstraintMember,
    DeclarationMacro, ExpressionMacro, Decorator };
struct SyntaxCompletion {
    Span replacement;
    CompletionPlace place{};
    std::vector<EditorSymbol> suggestions{};
    std::vector<Identifier> importPath{};
    std::vector<Identifier> selectedImports{};
    ImportKind importKind{};
    std::optional<Span> callOpen{};
    std::optional<Identifier> qualifier{};
    bool argumentLabel{}, incomplete{}, afterPublic{}, asynchronous{}, implicitReceiver{};
};
struct Module { std::vector<Function> functions; std::vector<Aggregate> aggregates; std::vector<Import> imports; std::vector<Expansion> expansions; std::vector<Trait> traits{}; std::vector<Enumeration> enums{}; std::vector<Extension> extensions{}; std::vector<std::vector<std::string>> importedExtensions{}; std::vector<ExceptionType> exceptions{}; std::vector<TypeAlias> aliases{}; };
Module parse(const std::vector<Source>& sources);
struct RecoveredSyntax { Module syntax; std::vector<Diagnostic> errors{}; bool incompleteDeclaration{}; };
void parseFile(Module& module, const Source& source, std::size_t file, std::vector<ExpressionExpansion>* expressions = nullptr, bool macroImplementation = false);
void validateMacroExpression(const Source& generated);
std::vector<SyntaxToken> syntaxTokens(const Source& source, bool comments = false, bool partial = false);
SyntaxCompletion completeSyntax(const Source& source, std::size_t offset);
Module indexFileSyntax(const Source& source, std::vector<ExpressionExpansion>* expressions = nullptr);
std::vector<OutlineSymbol> editorOutline(const Source& source, const Module& syntax);
RecoveredSyntax recoverFileSyntax(const Source& source, std::vector<ExpressionExpansion>* expressions = nullptr);
std::string editorDocumentation(const Source& source, std::size_t declarationStart);
std::string editorSpelling(const Source& source, const Identifier& name);
std::string editorModuleAlias(std::string_view name);
std::string editorIdentifier(std::string_view name);
EditorSignature describeFunction(const Source& source, const Function& function, std::string_view label,
    const std::vector<std::string>& parameterTypes = {}, std::string_view result = {}, std::size_t enclosingTypes = 0);

}
