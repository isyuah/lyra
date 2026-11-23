module;
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <variant>
#include <vector>

export module lyra.check;
export import lyra.syntax;

export namespace lyra {
struct Type {
    enum Kind { Unit, Int, Bool, String, Never, Record, Variable, Task, CPtr, Option, Result, List, Exception, Closure };
    Kind kind;
    std::size_t id{};
    constexpr Type(Kind kind = Unit, std::size_t id = 0) : kind(kind), id(id) {}
    bool operator==(const Type&) const = default;
};
struct Local { Type type; bool writable; std::optional<ExprId> worker{}; bool captured{}; };
enum class Callee { Function, Constructor, Print, Collect, Sum, ListAdd, TaskCancel, Exception, Indirect };
struct PatternSlot { std::size_t local; };
struct PatternProduct { bool operator==(const PatternProduct&) const = default; };
using PatternConstructor = std::variant<SumTag, bool, PatternProduct>;
struct CheckedPattern {
    std::variant<std::monostate, PatternSlot, PatternConstructor, std::int64_t, StringLiteral> node{};
    std::vector<CheckedPattern> parts{};
};
struct IteratorPlan { std::size_t open, next; };
struct CheckedExpr { Type type; std::size_t target{}; std::vector<std::size_t> argumentSlots{}; Callee callee{}; std::optional<Type> sourceType{}; std::vector<std::size_t> captures{}; std::vector<std::size_t> omittedDefaults{}; std::optional<std::size_t> conversion{}, errorConversion{}, functionRef{}; std::optional<Type> callableType{}; std::optional<IteratorPlan> iterator{}; std::vector<CheckedPattern> patterns{}; };
struct CheckedFunction {
    Function syntax;
    Type result;
    std::vector<Local> locals;
    std::vector<CheckedExpr> expressions;
    std::vector<std::optional<std::size_t>> defaultFunctions{};
    std::vector<std::optional<ExprId>> cleanupOwners{};
};
struct CheckedAggregate { Aggregate syntax; std::vector<Type> fields; };
struct SumTypes { Type success, failure; bool operator==(const SumTypes&) const = default; };
struct FunctionShape { std::vector<Type> parameters; Type result; bool operator==(const FunctionShape&) const = default; };
struct CheckedModule { std::deque<CheckedFunction> functions; std::optional<std::size_t> entry; std::vector<CheckedAggregate> aggregates; std::vector<std::string> variableNames; std::vector<Type> taskResults; std::vector<SumTypes> sumTypes; std::vector<Type> listElements; std::deque<FunctionShape> signatures{}; };
CheckedModule check(Module module, const std::vector<Source>& sources, bool requireEntry = true);
}
