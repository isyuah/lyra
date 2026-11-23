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

using Names = std::unordered_map<std::string, std::size_t>;

std::string typeName(const CheckedModule& module, Type type) {
    switch (type.kind) {
        case Type::Bool: return "Bool";
        case Type::Int: return "Int";
        case Type::Unit: return "Unit";
        case Type::Never: return "Never";
        case Type::String: return "String";
        case Type::Record: return module.aggregates[type.id].syntax.name.text;
    }
    throw std::logic_error("invalid Lyra type");
}

struct Symbols {
    const CheckedModule& module;
    const std::vector<Source>& sources;
    Names records, functions;
    std::vector<Names> fields, methods;

    Type resolve(const Source& source, const Identifier& name) const {
        for (Type type : {Type::Int, Type::Bool, Type::String, Type::Unit}) if (name.text == typeName(module, type)) return type;
        if (auto found = records.find(name.text); found != records.end()) return {Type::Record, found->second};
        throw at(source, name.span, "LYT0003", "找不到类型：" + name.text);
    }
};

Diagnostic mismatch(const CheckedModule& module, const Source& source, Span span, Type expected, Type actual) {
    auto error = at(source, span, "LYT0001", "期望 " + typeName(module, expected) + "，实际是 " + typeName(module, actual));
    error.expected = typeName(module, expected);
    error.actual = typeName(module, actual);
    return error;
}

struct Checker {
    CheckedFunction& fn;
    const Source& source;
    const CheckedModule& module;
    const Symbols& symbols;
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
        if (expected && type != *expected && type != Type::Never) throw mismatch(module, source, expr.span, *expected, type);
        return info.type = type;
    }

    Type infer(ExprId, std::int64_t, auto, bool) { return Type::Int; }
    Type infer(ExprId, bool, auto, bool) { return Type::Bool; }
    Type infer(ExprId, const StringLiteral&, auto, bool) { return Type::String; }

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
        if (binding.annotation) annotation = symbols.resolve(source, *binding.annotation);
        auto value = visit(binding.value, annotation);
        auto slot = fn.locals.size();
        declare(binding.name, slot);
        fn.locals.push_back({annotation.value_or(value), binding.writable});
        fn.expressions[id].target = slot;
        return value == Type::Never ? Type::Never : Type::Unit;
    }

    template<class GetName, class GetType>
    bool arguments(ExprId id, const Call& call, std::size_t count, GetName getName, GetType getType) {
        if (count != call.arguments.size()) throw at(source, call.name.span, "LYT0002", call.name.text + " 需要 " + std::to_string(count) + " 个实参");
        auto& info = fn.expressions[id];
        std::vector<bool> filled(count);
        bool named = false, exits = false;
        for (const auto& arg : call.arguments) {
            auto slot = info.argumentSlots.size();
            if (arg.label) {
                named = true;
                for (slot = 0; slot < count; ++slot) if (arg.label->text == getName(slot)) break;
                if (slot == count || filled[slot]) throw at(source, arg.label->span, "LYT0002", "未知或重复的命名实参：" + arg.label->text);
            } else if (named) throw at(source, fn.syntax.expressions[arg.value].span, "LYT0002", "位置实参须在命名实参之前");
            filled[slot] = true;
            info.argumentSlots.push_back(slot);
            exits |= visit(arg.value, getType(slot)) == Type::Never;
        }
        return exits;
    }

    std::size_t memberIndex(Type owner, const Identifier& name, const std::vector<Names>& tables) const {
        if (owner.kind == Type::Record) {
            const auto& names = tables[owner.id];
            if (auto found = names.find(name.text); found != names.end()) return found->second;
        }
        throw at(source, name.span, "LYN0001", typeName(module, owner) + " 没有成员：" + name.text);
    }

    void visible(std::size_t declaringFile, bool isPublic, const Identifier& name) const {
        if (!isPublic && symbols.sources[declaringFile].moduleId != source.moduleId) throw at(source, name.span, "LYN0003", "此成员是模块私有的：" + name.text);
    }

    Type infer(ExprId id, const Member& member, auto, bool) {
        auto owner = visit(member.receiver);
        if (owner == Type::Never) return owner;
        auto field = memberIndex(owner, member.name, symbols.fields);
        fn.expressions[id].target = field;
        const auto& record = module.aggregates[owner.id];
        visible(record.syntax.file, record.syntax.fields[field].isPublic, member.name);
        return record.fields[field];
    }

    Type infer(ExprId id, const Call& call, auto, bool) {
        auto& info = fn.expressions[id];
        if (!call.typeArguments.empty()) throw at(source, call.typeArguments.front().span, "LYT0002", "此调用不接收显式类型实参");
        if (call.receiver) {
            auto owner = visit(*call.receiver);
            if (owner == Type::Never) return owner;
            info.target = memberIndex(owner, call.name, symbols.methods);
            const auto& method = module.functions[info.target].syntax;
            visible(method.file, method.isPublic, call.name);
        } else if (call.name.text == "print") {
            info.callee = Callee::Print;
            bool exits = arguments(id, call, 1, [](auto) { return "value"; }, [](auto) { return std::nullopt; });
            auto arg = call.arguments.front().value;
            auto type = fn.expressions[arg].type;
            if (!exits && type != Type::Int && type != Type::String) throw at(source, fn.syntax.expressions[arg].span, "LYT0001", "print 需要 Int 或 String，实际是 " + typeName(module, type));
            return exits ? Type::Never : Type::Unit;
        } else if (call.name.text == "gcCollect") {
            info.callee = Callee::Collect;
            if (!call.arguments.empty()) throw at(source, call.name.span, "LYT0002", "gcCollect 不接收实参");
            return Type::Unit;
        } else if (auto record = symbols.records.find(call.name.text); record != symbols.records.end()) {
            info.callee = Callee::Constructor;
            info.target = record->second;
            const auto& decl = module.aggregates[info.target];
            for (const auto& field : decl.syntax.fields) visible(decl.syntax.file, field.isPublic, call.name);
            bool exits = arguments(id, call, decl.fields.size(), [&](auto slot) -> const auto& { return decl.syntax.fields[slot].name.text; }, [&](auto slot) { return decl.fields[slot]; });
            return exits ? Type{Type::Never} : Type{Type::Record, info.target};
        } else {
            auto found = symbols.functions.find(call.name.text);
            if (found == symbols.functions.end()) throw at(source, call.name.span, "LYN0001", "找不到函数：" + call.name.text);
            info.target = found->second;
        }
        const auto& target = module.functions[info.target];
        auto skip = static_cast<std::size_t>(call.receiver.has_value());
        bool exits = arguments(id, call, target.syntax.parameters.size() - skip,
                [&](auto slot) -> const auto& { return target.syntax.parameters[slot + skip].name.text; }, [&](auto slot) { return target.locals[slot + skip].type; });
        return exits ? Type{Type::Never} : target.result;
    }

    bool writable(ExprId id) const {
        const auto& expr = fn.syntax.expressions[id];
        if (std::holds_alternative<Identifier>(expr.node)) return fn.locals[fn.expressions[id].target].writable;
        if (auto member = std::get_if<Member>(&expr.node)) {
            const auto& owner = module.aggregates[fn.expressions[member->receiver].type.id].syntax;
            return owner.fields[fn.expressions[id].target].writable && (owner.reference || writable(member->receiver));
        }
        return false;
    }

    Type infer(ExprId, const Unary& unary, auto, bool) {
        return visit(unary.operand, unary.op == Op::Not ? Type::Bool : Type::Int);
    }

    Type infer(ExprId, const Binary& binary, auto, bool) {
        if (binary.op <= Op::RemSet) {
            const auto& place = fn.syntax.expressions[binary.left];
            auto type = visit(binary.left);
            if (type == Type::Never) { visit(binary.right); return type; }
            if (!writable(binary.left)) throw at(source, place.span, "LYT0004", "不能给此位置赋值；需要 var 绑定或可写字段");
            if (binary.op != Op::Set && type != Type::Int && !(binary.op == Op::AddSet && type == Type::String)) throw mismatch(module, source, place.span, Type::Int, type);
            return visit(binary.right, type) == Type::Never ? Type::Never : Type::Unit;
        }
        if (binary.op == Op::And || binary.op == Op::Or) {
            auto type = visit(binary.left, Type::Bool);
            visit(binary.right, Type::Bool);
            return type;
        }
        bool equality = binary.op == Op::Eq || binary.op == Op::Ne;
        auto left = visit(binary.left, equality || binary.op == Op::Add ? std::nullopt : std::optional(Type::Int));
        auto right = visit(binary.right, left == Type::Never ? std::nullopt : std::optional(left));
        if (left == Type::Never || right == Type::Never) return Type::Never;
        if (equality && left.kind == Type::Record && !module.aggregates[left.id].syntax.reference) throw at(source, binary.site, "LYT0006", typeName(module, left) + " 未提供 Eq，不能用 == 或 != 比较");
        if (binary.op == Op::Add && left == Type::String) return Type::String;
        if (binary.op == Op::Add && left != Type::Int) throw mismatch(module, source, binary.site, Type::Int, left);
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
            auto error = mismatch(module, source, site, *expected, type);
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
        else if (fn.result != Type::Unit) throw mismatch(module, source, fn.syntax.expressions[id].span, fn.result, Type::Unit);
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

CheckedModule check(Module module, const std::vector<Source>& sources, bool requireEntry) {
    CheckedModule checked{};
    Symbols symbols{checked, sources, {}, {}, {}, {}};
    for (auto& decl : module.aggregates) {
        auto id = checked.aggregates.size();
        for (Type builtin : {Type::Int, Type::Bool, Type::String, Type::Unit, Type::Never}) {
            if (decl.name.text == typeName(checked, builtin)) throw at(sources[decl.file], decl.name.span, "LYN0002", "类型名与内置类型冲突：" + decl.name.text);
        }
        if (decl.name.text == "print" || decl.name.text == "gcCollect" || !symbols.records.emplace(decl.name.text, id).second) throw at(sources[decl.file], decl.name.span, "LYN0002", "类型名称已声明：" + decl.name.text);
        checked.aggregates.push_back({std::move(decl), {}});
    }
    symbols.fields.resize(checked.aggregates.size());
    symbols.methods.resize(checked.aggregates.size());
    for (std::size_t id = 0; id < checked.aggregates.size(); ++id) {
        auto& record = checked.aggregates[id];
        const auto& source = sources[record.syntax.file];
        for (const auto& field : record.syntax.fields) {
            if (!symbols.fields[id].emplace(field.name.text, record.fields.size()).second) throw at(source, field.name.span, "LYN0002", "字段重复：" + field.name.text);
            record.fields.push_back(symbols.resolve(source, field.type));
        }
    }
    enum class Mark { Unseen, Visiting, Done };
    std::vector<Mark> marks(checked.aggregates.size());
    auto layout = [&](auto&& visit, std::size_t id) -> void {
        if (marks[id] == Mark::Done) return;
        marks[id] = Mark::Visiting;
        const auto& record = checked.aggregates[id];
        for (std::size_t slot = 0; slot < record.fields.size(); ++slot) {
            auto field = record.fields[slot];
            if (field.kind != Type::Record || checked.aggregates[field.id].syntax.reference) continue;
            if (marks[field.id] == Mark::Visiting) throw at(sources[record.syntax.file], record.syntax.fields[slot].type.span, "LYT0007", "值字段形成无限大小的内联布局：" + record.syntax.name.text + " → " + typeName(checked, field));
            visit(visit, field.id);
        }
        marks[id] = Mark::Done;
    };
    for (std::size_t id = 0; id < checked.aggregates.size(); ++id) layout(layout, id);
    for (auto& function : module.functions) {
        const auto& source = sources[function.file];
        if (!function.typeParameters.empty()) throw at(source, function.typeParameters.front().span, "LYT0003", "泛型函数需要先实例化");
        auto owner = function.owner ? std::optional(symbols.resolve(source, *function.owner).id) : std::nullopt;
        auto& names = owner ? symbols.methods[*owner] : symbols.functions;
        bool collision = owner ? symbols.fields[*owner].contains(function.name.text) : symbols.records.contains(function.name.text) || function.name.text == "print" || function.name.text == "gcCollect";
        if (collision || !names.emplace(function.name.text, checked.functions.size()).second) {
            throw at(source, function.name.span, "LYN0002", "函数名已声明：" + function.name.text);
        }
        auto result = symbols.resolve(source, function.result);
        if (!owner && requireEntry && function.name.text == "main") {
            if (!function.parameters.empty()) throw at(source, function.name.span, "LYT0002", "main 不能接收形参");
            if (result != Type::Unit) throw mismatch(checked, source, function.result.span, Type::Unit, result);
            checked.entry = checked.functions.size();
        }
        std::vector<Local> locals;
        for (const auto& parameter : function.parameters) locals.push_back({symbols.resolve(source, parameter.type), false});
        std::vector<CheckedExpr> expressions(function.expressions.size());
        checked.functions.push_back({std::move(function), result, std::move(locals), std::move(expressions)});
    }
    for (auto& fn : checked.functions) Checker{fn, sources[fn.syntax.file], checked, symbols, {}}.run();
    if (requireEntry && !checked.entry) throw Diagnostic{"LYN0001", "可执行包缺少入口 fn main(): Unit"};
    return checked;
}

}
