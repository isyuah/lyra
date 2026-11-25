module;
#include <compare>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module lyra.check;
export import lyra.syntax;

export namespace lyra {
struct Type {
    enum Kind { Unit, Int, UInt, Real, Bool, String, Char, Bytes, Never, Record, Enum, Variable, Task, CPtr, Option, Result, List, Exception, Closure, Any, Dyn, Error, TaskLocal, Projection, IntRange, Channel, CCallback, FrozenList };
    Kind kind;
    std::size_t id{};
    constexpr Type(Kind kind = Unit, std::size_t id = 0) : kind(kind), id(id) {}
    auto operator<=>(const Type&) const = default;
};
inline constexpr Type rangeFields[] = {Type::Int, Type::Int, Type::Bool};
inline constexpr const char* rangeFieldNames[] = {"start", "end", "inclusive"};
inline constexpr Type channelClosed{Type::Exception, 1};
inline constexpr Type boundsError{Type::Exception, 2};
inline constexpr Type arithmeticError{Type::Exception, 3};
inline constexpr std::string_view builtinExceptionNames[] = {"Exception", "ChannelClosed", "BoundsError", "ArithmeticError"};
inline constexpr std::size_t firstDeclaredException = sizeof(builtinExceptionNames) / sizeof(builtinExceptionNames[0]);
inline std::optional<Type> builtinException(std::string_view name) {
    for (std::size_t index = 0; index < firstDeclaredException; ++index)
        if (builtinExceptionNames[index] == name) return Type{Type::Exception, index};
    return std::nullopt;
}
struct ConstantUse { enum Kind { Global, Block, Uninstantiated }; Kind kind; std::size_t index{}; };
struct Local { Type type; bool writable; std::optional<ExprId> worker{}; bool captured{}; std::optional<Span> declaration{}; std::string name{}; std::optional<ConstantUse> constant{}; bool parameter{}; };
enum class Callee { Function, Constructor, Variant, Print, Collect, Sum, ListAdd, ListSlice, ListRemoveAt, FreezeList, TaskCancel, TaskCancelled, TaskCheckCancellation, AwaitOutcome, Exception, Indirect, ToReal, ToIntegerChecked, ToString, ToBytes, DecodeUtf8, Dynamic, BusinessError, ErrorSuppress, FailureInfo, LocalKey, LocalGet, WithLocal, ExtensionProperty, BoundMethod, ReflectTypeName, ReflectFieldCount, ReflectFieldName, ReflectField, Size, ValueEqual, ValueHash, ChannelCreate, ChannelIterator, ChannelSend, ChannelReceive, ChannelClose, Sleep, PtrAllocate, PtrNull, PtrIsNull, PtrFree, PtrOffset, PtrRead, PtrWrite, CallbackRegister, CallbackContext, CallbackFunction, CallbackClose, CallbackTakeError };
enum class ValueOperation { Equal, Hash };
struct PatternSlot { std::size_t local; };
struct PatternListRest { std::size_t local; };
struct PatternConstant { ConstantUse target; Type type; };
struct PatternProduct { bool operator==(const PatternProduct&) const = default; };
struct PatternClass { std::size_t owner; bool operator==(const PatternClass&) const = default; };
struct ListTag { bool cons; Type element; Type list; bool operator==(const ListTag&) const = default; };
struct CheckedAlternatives {};
struct CheckedIntegerRange { std::uint64_t lower, upper; bool inclusive, signedValues; };
struct VariantTag { std::size_t owner; unsigned index; bool operator==(const VariantTag&) const = default; };
using PatternConstructor = std::variant<SumTag, bool, PatternProduct, PatternClass, ListTag, VariantTag>;
struct CheckedPattern {
    std::variant<std::monostate, PatternSlot, PatternListRest, PatternConstant, PatternConstructor, std::uint64_t, CheckedIntegerRange, CheckedAlternatives, StringLiteral> node{};
    std::vector<CheckedPattern> parts{};
    Span site{};
};
enum class PatternUse { Match, Binding, Loop };
struct DeferredPatternCheck { std::size_t file; Span site; Type subject; std::vector<CheckedPattern> patterns; PatternUse use; };
struct MatchCoverage { std::size_t file; Type subject; struct Arm { CheckedPattern pattern; bool guarded; }; std::vector<Arm> arms; };
struct IteratorPlan { std::size_t open, next; std::optional<Type> awaited{}; };
struct CheckedIteration { Type element; std::optional<IteratorPlan> methods; };
struct CheckedCatch { Type type; std::optional<std::size_t> local; };
struct ErrorDisplay { std::optional<std::size_t> method; std::string fallback; };
enum class ErrorMember { Message, Original, Cause, Suppressed };
enum class ExceptionMember { Message, TaskFailures, CleanupFailures };
struct CheckedExpr { Type type; std::size_t target{}; std::vector<std::size_t> argumentSlots{}; Callee callee{}; std::optional<Type> sourceType{}; std::vector<std::size_t> captures{}; std::vector<std::size_t> omittedDefaults{}; std::optional<std::size_t> conversion{}, errorConversion{}, functionRef{}; std::optional<ConstantUse> constantRef{}; std::optional<Type> callableType{}; std::optional<CheckedIteration> iteration{}; std::vector<CheckedPattern> patterns{}; std::optional<double> realLiteral{}; std::optional<Type> narrowed{}; std::optional<std::string> boundTrait{}; std::optional<Type> testedType{}; bool boxedSpread{}; std::vector<std::size_t> interfaceMethods{}; bool interfaceAsync{}; std::vector<CheckedCatch> catches{}; ErrorDisplay errorDisplay{}, errorPromotion{}; std::vector<std::optional<std::size_t>> selectSlots{}; };
struct CheckedFunction {
    Function syntax;
    Type result;
    std::vector<Local> locals;
    std::vector<CheckedExpr> expressions;
    std::vector<std::optional<std::size_t>> defaultFunctions{};
    std::vector<std::optional<ExprId>> cleanupOwners{};
    std::vector<NameReference> references{};
    std::vector<SourceAnchor> calls{};
    std::vector<Type> throwsTypes{};
};
struct CheckedAggregate { Aggregate syntax; std::vector<Type> fields; std::size_t declaration; std::vector<Type> arguments; bool abstract; std::vector<std::optional<std::size_t>> defaultFunctions{}; };
struct CheckedEnum { Enumeration syntax; std::vector<std::vector<Type>> payloads; std::size_t declaration; std::vector<Type> arguments; bool abstract; };
struct SumTypes { Type success, failure; bool operator==(const SumTypes&) const = default; };
struct FunctionShape { std::vector<Type> parameters; Type result; bool operator==(const FunctionShape&) const = default; };
using ConstScalar = std::variant<std::monostate, bool, std::uint64_t, double, std::string>;
using MacroValue = std::variant<std::monostate, bool, std::int64_t, std::string, SyntaxRef>;
using MacroHost = std::function<MacroValue(std::string_view, std::span<const MacroValue>)>;
struct MacroExecution { std::size_t declaration, prelude; std::string target, argument; MacroHost host; SyntaxRef output{}; };
struct ConstRef { std::size_t index; bool operator==(const ConstRef&) const = default; };
using ConstValue = std::variant<ConstScalar, ConstRef>;
using ConstTag = std::variant<std::monostate, SumTag, VariantTag>;
struct ConstComposite { Type type; ConstTag tag; std::vector<ConstValue> fields; unsigned height{}; };
struct BlockConstant { std::size_t function; ExprId initializer; Type type; Identifier name; std::optional<ConstValue> value{}; };
struct TypeProjection { Type owner; std::string member; bool operator==(const TypeProjection&) const = default; };
struct DynamicType { std::size_t trait; std::vector<Type> bindings; bool operator==(const DynamicType&) const = default; };
struct CheckedModule { std::deque<CheckedFunction> functions; std::optional<std::size_t> entry; std::deque<CheckedAggregate> aggregates; std::vector<std::string> variableNames; std::vector<Type> taskResults; std::vector<SumTypes> sumTypes; std::vector<Type> listElements; std::deque<FunctionShape> signatures{}; std::deque<CheckedEnum> enums{}; std::vector<Trait> traits{}; std::vector<ExceptionType> exceptions{}; std::vector<Type> taskLocalValues{}; std::vector<std::optional<ConstValue>> constants{}; std::vector<TypeProjection> projections{}; std::deque<ConstComposite> constantComposites{}; std::map<std::pair<Type, ValueOperation>, std::size_t> valueOverrides{}; std::vector<DeferredPatternCheck> deferredPatternChecks{}; std::vector<Type> channelElements{}; std::vector<DynamicType> dynamicTypes{}; std::vector<MatchCoverage> matchCoverage{}; std::vector<Diagnostic> warnings{}; std::vector<BlockConstant> blockConstants{}; };

struct InferredTypeHint { SourceAnchor anchor; std::string type; };
struct EditorDescription { SourceAnchor anchor; std::string detail, documentation; };
enum EditorModifier : unsigned { Declaration = 1, Readonly = 2, StaticMember = 4, AsyncFunction = 8 };
struct EditorBinding { EditorKind kind; std::optional<SourceAnchor> typeDefinition{}; unsigned modifiers{}; };
struct EditorProbe {
    std::filesystem::path path;
    std::string marker;
    std::vector<EditorSymbol> members{};
    std::vector<EditorSignature> signatures{};
    std::vector<EditorSymbol> labels{};
    bool namespaceReceiver{}, receiverChecked{};
};
struct EditorProbeFinished {};
struct EditorIndex {
    std::vector<NameReference> references;
    std::vector<InferredTypeHint> inferredTypes;
    std::vector<EditorDescription> descriptions;
    std::map<SourceAnchor, EditorBinding> bindings;
    EditorProbe* probe{};
};
std::vector<EditorSymbol> editorBuiltins(bool types, bool constraints = false);
const std::optional<ConstValue>& constantValue(const CheckedModule& module, ConstantUse target);
std::string typeName(const CheckedModule& module, Type type, bool canonical = false);
std::optional<Type> hiddenType(const CheckedModule& module, Type type);
CheckedModule check(Module module, const std::vector<Source>& sources, bool requireEntry = true, MacroExecution* macro = nullptr, EditorIndex* editor = nullptr);
void checkForDiagnostics(Module module, const std::vector<Source>& sources, bool requireEntry, AnalysisDiagnostics& diagnostics, EditorIndex* editor = nullptr);
}
