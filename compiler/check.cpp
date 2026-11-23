module;
#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

module lyra.check;

namespace lyra {
namespace {

const char* typeName(Type type) {
    switch (type) {
        case Type::Bool: return "Bool";
        case Type::Int: return "Int";
        case Type::Unit: return "Unit";
        case Type::Never: return "Never";
    }
    throw std::logic_error("invalid Lyra type");
}

Type resolveType(const Source& source, const Identifier& name) {
    for (Type type : {Type::Int, Type::Bool, Type::Unit}) if (name.text == typeName(type)) return type;
    throw at(source, name.span, "LYT0003", "找不到类型：" + name.text);
}

Diagnostic mismatch(const Source& source, Span span, Type expected, Type actual) {
    auto error = at(source, span, "LYT0001", std::string("期望 ") + typeName(expected) + "，实际是 " + typeName(actual));
    error.expected = typeName(expected);
    error.actual = typeName(actual);
    return error;
}

using Names = std::unordered_map<std::string, std::size_t>;

struct Checker {
    CheckedFunction& fn;
    const Source& source;
    const CheckedModule& module;
    const Names& functions;
    std::vector<Names> scopes;
    unsigned loops = 0;

    void declare(const Identifier& name, std::size_t slot) {
        if (!scopes.back().emplace(name.text, slot).second) throw at(source, name.span, "LYN0002", "名称已在此作用域声明：" + name.text);
    }

    void run() {
        scopes.emplace_back();
        for (std::size_t index = 0; index < fn.syntax.parameters.size(); ++index) declare(fn.syntax.parameters[index].name, index);
        visit(fn.syntax.body, fn.result);
    }

    Type visit(ExprId id, std::optional<Type> expected = {}, bool discard = false) {
        const auto& expr = fn.syntax.expressions[id];
        auto& info = fn.expressions[id];
        auto type = std::visit([&](const auto& node) { return infer(id, node, expected, discard); }, expr.node);
        if (expected && type != *expected && type != Type::Never) throw mismatch(source, expr.span, *expected, type);
        return info.type = type;
    }

    Type infer(ExprId, std::int64_t, auto, bool) { return Type::Int; }
    Type infer(ExprId, bool, auto, bool) { return Type::Bool; }

    Type infer(ExprId id, const Identifier& name, auto, bool) {
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
            if (auto found = scope->find(name.text); found != scope->end()) {
                fn.expressions[id].target = found->second;
                return fn.locals[found->second].type;
            }
        }
        throw at(source, name.span, "LYN0001", "找不到绑定：" + name.text);
    }

    Type infer(ExprId id, const Binding& binding, auto, bool) {
        std::optional<Type> annotation;
        if (binding.annotation) annotation = resolveType(source, *binding.annotation);
        auto value = visit(binding.value, annotation);
        auto slot = fn.locals.size();
        declare(binding.name, slot);
        fn.locals.push_back({annotation.value_or(value), binding.writable});
        fn.expressions[id].target = slot;
        return value == Type::Never ? Type::Never : Type::Unit;
    }

    Type infer(ExprId id, const Call& call, auto, bool) {
        bool print = call.name.text == "print";
        auto found = functions.find(call.name.text);
        if (!print && found == functions.end()) throw at(source, call.name.span, "LYN0001", "找不到函数：" + call.name.text);
        auto* target = print ? nullptr : &module.functions[found->second];
        auto count = print ? 1 : target->syntax.parameters.size();
        if (count != call.arguments.size()) throw at(source, call.name.span, "LYT0002", call.name.text + " 需要 " + std::to_string(count) + " 个实参");
        auto& info = fn.expressions[id];
        if (!print) info.target = found->second;
        std::vector<bool> filled(count);
        bool named = false, exits = false;
        for (const auto& arg : call.arguments) {
            auto slot = info.argumentSlots.size();
            if (arg.label) {
                named = true;
                for (slot = 0; slot < count; ++slot) if (arg.label->text == (print ? "value" : target->syntax.parameters[slot].name.text)) break;
                if (slot == count || filled[slot]) throw at(source, arg.label->span, "LYT0002", "未知或重复的命名实参：" + arg.label->text);
            } else if (named) throw at(source, fn.syntax.expressions[arg.value].span, "LYT0002", "位置实参须在命名实参之前");
            filled[slot] = true;
            info.argumentSlots.push_back(slot);
            exits |= visit(arg.value, print ? Type::Int : target->locals[slot].type) == Type::Never;
        }
        return exits ? Type::Never : print ? Type::Unit : target->result;
    }

    Type infer(ExprId, const Unary& unary, auto, bool) {
        return visit(unary.operand, unary.op == Op::Not ? Type::Bool : Type::Int);
    }

    Type infer(ExprId, const Binary& binary, auto, bool) {
        if (binary.op <= Op::RemSet) {
            const auto& place = fn.syntax.expressions[binary.left];
            if (!std::holds_alternative<Identifier>(place.node)) throw at(source, place.span, "LYT0004", "赋值左侧需要可写的位置");
            auto type = visit(binary.left, binary.op == Op::Set ? std::nullopt : std::optional(Type::Int));
            if (!fn.locals[fn.expressions[binary.left].target].writable) throw at(source, place.span, "LYT0004", "不能重新赋值 val 或形参；可变绑定请用 var");
            return visit(binary.right, type) == Type::Never ? Type::Never : Type::Unit;
        }
        if (binary.op == Op::And || binary.op == Op::Or) {
            auto type = visit(binary.left, Type::Bool);
            visit(binary.right, Type::Bool);
            return type;
        }
        bool equality = binary.op == Op::Eq || binary.op == Op::Ne;
        auto left = visit(binary.left, equality ? std::nullopt : std::optional(Type::Int));
        auto right = visit(binary.right, left == Type::Never ? std::nullopt : std::optional(left));
        if (left == Type::Never || right == Type::Never) return Type::Never;
        return binary.op <= Op::Ge ? Type::Bool : Type::Int;
    }

    Type infer(ExprId id, const Block& block, std::optional<Type> expected, bool discard) {
        if (id != fn.syntax.body) scopes.emplace_back();
        bool live = true;
        Type type = Type::Unit;
        Span site = block.end;
        bool semiFix = false;
        for (const auto& statement : block.statements) {
            bool tail = !discard && &statement == &block.statements.back() && !statement.semicolon && live;
            auto value = visit(statement.expression, tail ? expected : std::nullopt, !tail);
            live &= value != Type::Never;
            type = statement.semicolon ? Type::Unit : value;
            site = statement.semicolon.value_or(fn.syntax.expressions[statement.expression].span);
            semiFix = statement.semicolon && expected == value;
        }
        if (id != fn.syntax.body) scopes.pop_back();
        if (!live) return Type::Never;
        if (expected && *expected != type) {
            auto error = mismatch(source, site, *expected, type);
            if (semiFix) error.hint = "去掉末尾分号，让最后的表达式成为返回值。";
            throw error;
        }
        return discard ? Type::Unit : type;
    }

    Type infer(ExprId id, const Conditional& branch, std::optional<Type> expected, bool discard) {
        auto condition = visit(branch.condition, Type::Bool);
        if (!branch.no && !discard && expected != Type::Unit) throw at(source, fn.syntax.expressions[id].span, "LYT0005", "值位置的 if 需要 else 分支");
        bool ignore = discard || !branch.no;
        auto yes = visit(branch.yes, ignore ? std::nullopt : expected, ignore);
        auto no = branch.no ? visit(*branch.no, !ignore && yes != Type::Never ? std::optional(yes) : expected, ignore) : Type::Unit;
        if (condition == Type::Never || (yes == Type::Never && no == Type::Never)) return Type::Never;
        return ignore ? Type::Unit : yes == Type::Never ? no : yes;
    }

    Type infer(ExprId id, const Return& ret, std::optional<Type>, bool) {
        if (ret.value) visit(*ret.value, fn.result);
        else if (fn.result != Type::Unit) throw mismatch(source, fn.syntax.expressions[id].span, fn.result, Type::Unit);
        return Type::Never;
    }

    Type infer(ExprId, const While& loop, auto, bool) {
        auto condition = visit(loop.condition, Type::Bool);
        ++loops;
        visit(loop.body, {}, true);
        --loops;
        return condition == Type::Never ? Type::Never : Type::Unit;
    }

    Type infer(ExprId id, Jump, auto, bool) {
        if (!loops) throw at(source, fn.syntax.expressions[id].span, "LYT0005", "break 和 continue 只可用于循环体");
        return Type::Never;
    }
};

}

CheckedModule check(Module module, const std::vector<Source>& sources) {
    CheckedModule checked{};
    Names names;
    for (auto& function : module) {
        const auto& source = sources[function.file];
        if (function.name.text == "print" || !names.emplace(function.name.text, checked.functions.size()).second) {
            throw at(source, function.name.span, "LYN0002", "函数名已声明：" + function.name.text);
        }
        auto result = resolveType(source, function.result);
        if (function.name.text == "main") {
            if (!function.parameters.empty()) throw at(source, function.name.span, "LYT0002", "main 不能接收形参");
            if (result != Type::Unit) throw mismatch(source, function.result.span, Type::Unit, result);
        }
        std::vector<Local> locals;
        for (const auto& parameter : function.parameters) locals.push_back({resolveType(source, parameter.type), false});
        std::vector<CheckedExpr> expressions(function.expressions.size());
        checked.functions.push_back({std::move(function), result, std::move(locals), std::move(expressions)});
    }
    for (auto& fn : checked.functions) Checker{fn, sources[fn.syntax.file], checked, names, {}}.run();
    if (!names.contains("main")) throw Diagnostic{"LYN0001", "可执行包缺少入口 fn main(): Unit"};
    checked.entry = names.at("main");
    return checked;
}

}
