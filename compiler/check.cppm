module;
#include <compare>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

export module lyra.check;
export import lyra.syntax;

export namespace lyra {
struct Type {
    enum Kind { Unit, Int, UInt, Real, Bool, String, Char, Bytes, Never, Record, Enum, Variable, Task, CPtr, Option, Result, List, Exception, Closure, Any, Dyn, Error, TaskLocal, Projection, IntRange, Channel, CCallback };
    Kind kind;
    std::size_t id{};
    constexpr Type(Kind kind = Unit, std::size_t id = 0) : kind(kind), id(id) {}
    auto operator<=>(const Type&) const = default;
};
inline constexpr Type rangeFields[] = {Type::Int, Type::Int, Type::Bool};
inline constexpr const char* rangeFieldNames[] = {"start", "end", "inclusive"};
inline constexpr Type channelClosed{Type::Exception, 1};
inline constexpr std::size_t firstDeclaredException = channelClosed.id + 1;
struct Local { Type type; bool writable; std::optional<ExprId> worker{}; bool captured{}; std::optional<Span> declaration{}; std::string name{}; };
enum class Callee { Function, Constructor, Variant, Print, Collect, Sum, ListAdd, ListSlice, TaskCancel, TaskCancelled, TaskCheckCancellation, Exception, Indirect, ToReal, ToIntegerChecked, ToString, ToBytes, DecodeUtf8, Dynamic, BusinessError, ErrorSuppress, FailureInfo, LocalKey, LocalGet, WithLocal, ExtensionProperty, ReflectTypeName, ReflectFieldCount, ReflectFieldName, ReflectField, Size, ValueEqual, ValueHash, ChannelCreate, ChannelSend, ChannelReceive, ChannelClose, Sleep, PtrAllocate, PtrNull, PtrIsNull, PtrFree, PtrOffset, PtrRead, PtrWrite, CallbackRegister, CallbackContext, CallbackFunction, CallbackClose, CallbackTakeError };
enum class ValueOperation { Equal, Hash };
struct PatternSlot { std::size_t local; };
struct PatternListRest { std::size_t local; };
struct PatternConstant { std::size_t target; Type type; };
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
enum class PatternUse { Match, Binding };
struct DeferredPatternCheck { std::size_t file; Span site; Type subject; std::vector<CheckedPattern> patterns; PatternUse use; };
struct MatchCoverage { std::size_t file; Type subject; struct Arm { CheckedPattern pattern; bool guarded; }; std::vector<Arm> arms; };
struct IteratorPlan { std::size_t open, next; };
struct CheckedCatch { Type type; std::optional<std::size_t> local; };
struct ErrorDisplay { std::optional<std::size_t> method; std::string fallback; };
enum class ErrorMember { Message, Original, Cause, Suppressed };
enum class ExceptionMember { Message, TaskFailures, CleanupFailures };
struct CheckedExpr { Type type; std::size_t target{}; std::vector<std::size_t> argumentSlots{}; Callee callee{}; std::optional<Type> sourceType{}; std::vector<std::size_t> captures{}; std::vector<std::size_t> omittedDefaults{}; std::optional<std::size_t> conversion{}, errorConversion{}, functionRef{}, constantRef{}; std::optional<Type> callableType{}; std::optional<IteratorPlan> iterator{}; std::vector<CheckedPattern> patterns{}; std::optional<double> realLiteral{}; std::optional<Type> narrowed{}; std::optional<std::string> boundTrait{}; std::optional<Type> testedType{}; bool boxedSpread{}; std::vector<std::size_t> interfaceMethods{}; bool interfaceAsync{}; std::vector<CheckedCatch> catches{}; ErrorDisplay errorDisplay{}, errorPromotion{}; std::vector<std::optional<std::size_t>> selectSlots{}; };
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
struct ConstRef { std::size_t index; bool operator==(const ConstRef&) const = default; };
using ConstValue = std::variant<ConstScalar, ConstRef>;
using ConstTag = std::variant<std::monostate, SumTag, VariantTag>;
struct ConstComposite { Type type; ConstTag tag; std::vector<ConstValue> fields; unsigned height{}; };
struct TypeProjection { Type owner; std::string member; bool operator==(const TypeProjection&) const = default; };
struct DynamicType { std::size_t trait; std::vector<Type> bindings; bool operator==(const DynamicType&) const = default; };
struct CheckedModule { std::deque<CheckedFunction> functions; std::optional<std::size_t> entry; std::deque<CheckedAggregate> aggregates; std::vector<std::string> variableNames; std::vector<Type> taskResults; std::vector<SumTypes> sumTypes; std::vector<Type> listElements; std::deque<FunctionShape> signatures{}; std::deque<CheckedEnum> enums{}; std::vector<NameReference> references{}; std::vector<Trait> traits{}; std::vector<ExceptionType> exceptions{}; std::vector<Type> taskLocalValues{}; std::vector<std::optional<ConstValue>> constants{}; std::vector<TypeProjection> projections{}; std::deque<ConstComposite> constantComposites{}; std::map<std::pair<Type, ValueOperation>, std::size_t> valueOverrides{}; std::vector<DeferredPatternCheck> deferredPatternChecks{}; std::vector<Type> channelElements{}; std::vector<DynamicType> dynamicTypes{}; std::vector<MatchCoverage> matchCoverage{}; std::vector<Diagnostic> warnings{}; };
std::string typeName(const CheckedModule& module, Type type, bool canonical = false);
std::optional<Type> hiddenType(const CheckedModule& module, Type type);
CheckedModule check(Module module, const std::vector<Source>& sources, bool requireEntry = true);
}
