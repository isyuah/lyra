module;
#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

export module lyra.initialization;
import lyra.check;

namespace lyra {

namespace {
enum class FieldState { Unset, Maybe, Ready };
using State = std::vector<FieldState>;
using Path = std::optional<State>;

void merge(Path& into, Path from) {
    if (!from) return;
    if (!into) { into = std::move(from); return; }
    for (std::size_t slot = 0; slot < into->size(); ++slot)
        if ((*into)[slot] != (*from)[slot]) (*into)[slot] = FieldState::Maybe;
}

struct Flow {
    const CheckedFunction& fn;
    const CheckedAggregate& record;
    const Source& source;

    Span site(ExprId id) const { return fn.syntax.expressions[id].span; }
    bool isReceiver(ExprId id) const { return std::holds_alternative<This>(fn.syntax.expressions[id].node); }

    void requireReady(const State& state, Span span, const char* action) const {
        auto missing = std::ranges::find_if(state, [](auto value) { return value != FieldState::Ready; });
        if (missing != state.end())
            throw at(source, span, "LYT0017", std::string(action) + "前必须初始化字段 " + record.syntax.fields[missing - state.begin()].name.text);
    }

    bool eval(ExprId id, State& state, bool restricted = false) const {
        const auto& expression = fn.syntax.expressions[id];
        return std::visit([&](const auto& node) { return walk(id, node, state, restricted); }, expression.node) && fn.expressions[id].type != Type::Never;
    }

    Path branch(ExprId id, State state, bool restricted) const {
        if (eval(id, state, restricted)) return state;
        return {};
    }

    bool join(State& state, Path next) const {
        if (!next) return false;
        state = std::move(*next);
        return true;
    }

    bool walk(ExprId, const auto&, State&, bool) const { return true; }
    bool walk(ExprId id, This, State& state, bool) const {
        requireReady(state, site(id), "使用 this");
        return true;
    }

    bool walk(ExprId id, const Member& member, State& state, bool restricted) const {
        if (fn.expressions[id].callee == Callee::Variant) return true;
        if (!isReceiver(member.receiver)) return eval(member.receiver, state, restricted);
        auto field = fn.expressions[id].target;
        if (state[field] != FieldState::Ready) throw at(source, member.name.span, "LYT0017", "字段尚未在所有路径初始化：" + record.syntax.fields[field].name.text);
        return true;
    }

    bool walk(ExprId id, const Call& call, State& state, bool restricted) const {
        if (call.receiver && fn.expressions[id].callee != Callee::Variant && !eval(*call.receiver, state, restricted)) return false;
        if (call.function && !eval(*call.function, state, restricted)) return false;
        for (const auto& argument : call.arguments) if (!eval(argument.value, state, restricted)) return false;
        return true;
    }

    bool walk(ExprId, const Binary& binary, State& state, bool restricted) const {
        if (binary.op == Op::Set) {
            auto* field = std::get_if<Member>(&fn.syntax.expressions[binary.left].node);
            if (field && isReceiver(field->receiver)) {
                if (!eval(binary.right, state, restricted)) return false;
                auto slot = fn.expressions[binary.left].target;
                if (!record.syntax.fields[slot].writable && state[slot] != FieldState::Unset)
                    throw at(source, field->name.span, "LYT0017", "val 字段只能初始化一次：" + field->name.text);
                if (restricted && state[slot] != FieldState::Ready)
                    throw at(source, field->name.span, "LYT0017", "不能在循环、catch 保护的 try 主体或模式守卫内首次初始化字段：" + field->name.text);
                state[slot] = FieldState::Ready;
                return true;
            }
        }
        if (!eval(binary.left, state, restricted)) return false;
        if (binary.op != Op::And && binary.op != Op::Or) return eval(binary.right, state, restricted);
        Path next = state;
        merge(next, branch(binary.right, state, restricted));
        return join(state, std::move(next));
    }

    bool walk(ExprId, const Block& block, State& state, bool restricted) const {
        for (const auto& statement : block.statements) if (!eval(statement.expression, state, restricted)) return false;
        return true;
    }

    bool walk(ExprId, const Conditional& conditional, State& state, bool restricted) const {
        if (!eval(conditional.condition, state, restricted)) return false;
        auto next = branch(conditional.yes, state, restricted);
        merge(next, conditional.no ? branch(*conditional.no, state, restricted) : Path(state));
        return join(state, std::move(next));
    }

    bool walk(ExprId, const Match& match, State& state, bool restricted) const {
        if (!eval(match.subject, state, restricted)) return false;
        Path next;
        for (const auto& arm : match.arms) {
            auto armState = state;
            if (!arm.guard || eval(*arm.guard, armState, true)) merge(next, branch(arm.value, std::move(armState), restricted));
        }
        return join(state, std::move(next));
    }

    bool walk(ExprId, const Attempt& attempt, State& state, bool restricted) const {
        auto next = branch(attempt.body, state, true);
        for (const auto& arm : attempt.arms) {
            auto entered = state;
            if (!arm.guard || eval(*arm.guard, entered, true)) merge(next, branch(arm.body, std::move(entered), restricted));
        }
        return join(state, std::move(next));
    }

    bool walk(ExprId, const While& loop, State& state, bool) const {
        if (!eval(loop.condition, state, true)) return false;
        branch(loop.body, state, true);
        return true;
    }

    bool walk(ExprId, const For& loop, State& state, bool restricted) const {
        if (!eval(loop.input, state, restricted) || (loop.range && !eval(loop.range->end, state, restricted))) return false;
        branch(loop.body, state, true);
        return true;
    }

    bool walk(ExprId id, const Return&, State& state, bool) const { requireReady(state, site(id), "结束 init"); return false; }
    bool walk(ExprId, const Raise& raise, State& state, bool restricted) const {
        if (raise.value) eval(*raise.value, state, restricted);
        return false;
    }
    bool walk(ExprId, Jump, State&, bool) const { return false; }

    void capture(ExprId id, const State& state) const {
        const auto& captures = fn.expressions[id].captures;
        if (std::ranges::find(captures, fn.syntax.parameters.size()) != captures.end()) requireReady(state, site(id), "捕获 this");
    }

    bool walk(ExprId id, const Lambda&, State& state, bool) const { capture(id, state); return true; }
    bool walk(ExprId id, const Blocking&, State& state, bool) const { capture(id, state); return true; }
    bool walk(ExprId id, const AsyncBlock&, State& state, bool) const { capture(id, state); return true; }
    bool walk(ExprId id, const Deferred&, State& state, bool) const { capture(id, state); return true; }
    bool walk(ExprId, const Binding& binding, State& state, bool restricted) const { return eval(binding.value, state, restricted); }
    bool walk(ExprId, const Unary& unary, State& state, bool restricted) const { return eval(unary.operand, state, restricted); }
    bool walk(ExprId, const TypeOperation& operation, State& state, bool restricted) const { return eval(operation.value, state, restricted); }
    bool walk(ExprId, const DisplayValue& display, State& state, bool restricted) const { return eval(display.value, state, restricted); }
    bool walk(ExprId, const Propagate& propagation, State& state, bool restricted) const { return eval(propagation.operand, state, restricted); }
    bool walk(ExprId, const TaskOp& op, State& state, bool restricted) const { return eval(op.operand, state, restricted); }
    bool walk(ExprId, const Index& index, State& state, bool restricted) const { return eval(index.receiver, state, restricted) && eval(index.offset, state, restricted); }
    bool walk(ExprId, const ListLiteral& list, State& state, bool restricted) const {
        return std::ranges::all_of(list.elements, [&](auto element) { return eval(element, state, restricted); });
    }
    bool walk(ExprId, const TextTemplate& text, State& state, bool restricted) const {
        return std::ranges::all_of(text.parts, [&](auto part) { return eval(part, state, restricted); });
    }
};
}

export void checkInitialization(const CheckedFunction& fn, const CheckedAggregate& record, const Source& source) {
    Flow flow{fn, record, source};
    State state(record.fields.size(), FieldState::Unset);
    if (flow.eval(fn.syntax.body, state)) flow.requireReady(state, std::get<Block>(fn.syntax.expressions[fn.syntax.body].node).end, "结束 init");
}

}
