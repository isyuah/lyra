module;
#include <deque>
#include <optional>
#include <string>
#include <vector>

export module lyra.check;
export import lyra.syntax;

export namespace lyra {
struct Type {
    enum Kind { Unit, Int, Bool, String, Never, Record, Variable };
    Kind kind;
    std::size_t id{};
    constexpr Type(Kind kind = Unit, std::size_t id = 0) : kind(kind), id(id) {}
    bool operator==(const Type&) const = default;
};
struct Local { Type type; bool writable; };
enum class Callee { Function, Constructor, Print, Collect };
struct CheckedExpr { Type type; std::size_t target{}; std::vector<std::size_t> argumentSlots{}; Callee callee{}; };
struct CheckedFunction {
    Function syntax;
    Type result;
    std::vector<Local> locals;
    std::vector<CheckedExpr> expressions;
};
struct CheckedAggregate { Aggregate syntax; std::vector<Type> fields; };
struct CheckedModule { std::deque<CheckedFunction> functions; std::optional<std::size_t> entry; std::vector<CheckedAggregate> aggregates; std::vector<std::string> variableNames; };
CheckedModule check(Module module, const std::vector<Source>& sources, bool requireEntry = true);
}
