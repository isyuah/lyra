module;
#include <vector>

export module lyra.check;
export import lyra.syntax;

export namespace lyra {
enum class Type { Unit, Int, Bool, Never };
struct Local { Type type; bool writable; };
struct CheckedExpr { Type type; std::size_t target{}; std::vector<std::size_t> argumentSlots{}; };
struct CheckedFunction {
    Function syntax;
    Type result;
    std::vector<Local> locals;
    std::vector<CheckedExpr> expressions;
};
struct CheckedModule { std::vector<CheckedFunction> functions; std::size_t entry; };
CheckedModule check(Module module, const std::vector<Source>& sources);
}
