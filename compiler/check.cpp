module;
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <utility>
#include <variant>
#include <vector>

module lyra.check;
import lyra.generics;
import lyra.foreign;
import lyra.conversions;
import lyra.collections;

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
        case Type::Exception: return "Exception";
        case Type::CPtr: return "CPtr";
        case Type::Record: return module.aggregates[type.id].syntax.name.text;
        case Type::Variable: return module.variableNames[type.id];
        case Type::Task: return "Task<" + typeName(module, module.taskResults[type.id]) + ">";
        case Type::List: return "List<" + typeName(module, module.listElements[type.id]) + ">";
        case Type::Option: return "Option<" + typeName(module, module.sumTypes[type.id].success) + ">";
        case Type::Result: return "Result<" + typeName(module, module.sumTypes[type.id].success) + "," + typeName(module, module.sumTypes[type.id].failure) + ">";
    }
    throw std::logic_error("invalid Lyra type");
}

struct Callable { std::size_t target; std::optional<Function> generic; std::vector<Type> variables; };

Function defaultFunction(const Function& original, std::size_t slot) {
    auto syntax = original;
    const auto& parameter = original.parameters[slot];
    syntax.name.text = (original.owner ? original.owner->text + "." : "") + original.name.text + "$default$" + parameter.name.text;
    syntax.name.span = parameter.name.span;
    syntax.body = *parameter.defaultValue;
    syntax.result = parameter.type;
    syntax.owner.reset();
    syntax.isAsync = false;
    syntax.parameters.resize(slot);
    for (auto& earlier : syntax.parameters) earlier.defaultValue.reset();
    return syntax;
}

struct Symbols {
    CheckedModule& module;
    const std::vector<Source>& sources;
    Names records, functions;
    std::vector<Names> fields, methods;
    std::vector<Callable> callables;
    std::map<GenericInstanceKey, std::size_t> specializations;
    std::vector<DirectConversion> conversions{};

    Type unaryType(Type::Kind kind, std::vector<Type>& types, Type result) const {
        auto found = std::find(types.begin(), types.end(), result);
        auto index = static_cast<std::size_t>(found - types.begin());
        if (found == types.end()) types.push_back(result);
        return {kind, index};
    }

    Type task(Type result) const { return unaryType(Type::Task, module.taskResults, result); }
    Type list(Type element) const { return unaryType(Type::List, module.listElements, element); }

    Type sum(Type::Kind kind, Type success, Type failure = Type::Unit) const {
        auto& types = module.sumTypes;
        SumTypes fields{success, failure};
        auto found = std::find(types.begin(), types.end(), fields);
        auto index = static_cast<std::size_t>(found - types.begin());
        if (found == types.end()) types.push_back(fields);
        return {kind, index};
    }

    std::optional<Type> project(Type value, const std::string& name, std::size_t slot) const {
        if (name == "Task" && value.kind == Type::Task && slot == 0) return module.taskResults[value.id];
        if (name == "List" && value.kind == Type::List && slot == 0) return module.listElements[value.id];
        if (name == "Option" && value.kind == Type::Option && slot == 0) return module.sumTypes[value.id].success;
        if (name == "Result" && value.kind == Type::Result && slot < 2) return slot ? module.sumTypes[value.id].failure : module.sumTypes[value.id].success;
        return std::nullopt;
    }

    Type resolveForm(const TypeForm& form, const Source& source, Span span, const Function* signature, std::span<const Type> types) const {
        auto child = [&](std::size_t slot) { return resolveForm(form.arguments[slot], source, span, signature, types); };
        if (form.arguments.size() == 1 && form.name == "Task") return task(child(0));
        if (form.arguments.size() == 1 && form.name == "List") return list(child(0));
        if (form.arguments.size() == 1 && form.name == "Option") return sum(Type::Option, child(0));
        if (form.arguments.size() == 2 && form.name == "Result") return sum(Type::Result, child(0), child(1));
        if (form.arguments.empty()) {
            if (signature) if (auto parameter = genericParameter(*signature, form.name)) return types[*parameter];
            for (Type type : {Type::Int, Type::Bool, Type::String, Type::Unit, Type::CPtr, Type::Exception}) if (form.name == typeName(module, type)) return type;
            if (auto found = records.find(form.name); found != records.end()) return {Type::Record, found->second};
        }
        throw at(source, span, "LYT0003", "找不到类型或类型实参数量不符：" + form.name);
    }

    Type resolve(const Source& source, const Identifier& name) const {
        return resolveForm(typeForm(name.text), source, name.span, nullptr, {});
    }

    Type resolve(const Function& signature, const Identifier& name, std::span<const Type> types) const {
        return resolveForm(typeForm(name.text), sources[signature.file], name.span, &signature, types);
    }

    CheckedFunction prepare(Function syntax, std::span<const Type> arguments = {}) const {
        auto result = resolve(syntax, syntax.result, arguments);
        std::vector<Local> locals;
        for (const auto& param : syntax.parameters) {
            auto type = resolve(syntax, param.type, arguments);
            locals.push_back({param.variadic ? list(type) : type, false});
        }
        std::vector<CheckedExpr> expressions(syntax.expressions.size());
        return {std::move(syntax), result, std::move(locals), std::move(expressions)};
    }

    void addConcrete(CheckedFunction function) {
        auto& target = module.functions.emplace_back(std::move(function));
        target.defaultFunctions.resize(target.syntax.parameters.size());
        for (std::size_t slot = 0; slot < target.syntax.parameters.size(); ++slot) if (target.syntax.parameters[slot].defaultValue) {
            target.defaultFunctions[slot] = module.functions.size();
            module.functions.push_back(prepare(defaultFunction(target.syntax, slot)));
        }
    }

    std::size_t instantiate(std::size_t declaration, const std::vector<Type>& arguments, const Source& source, Span site) {
        GenericInstanceKey key{declaration, {}};
        for (auto type : arguments) key.arguments.push_back(typeName(module, type));
        if (auto found = specializations.find(key); found != specializations.end()) return found->second;
        if (specializations.size() >= 4096) throw at(source, site, "LYG0004", "此编译单元的泛型函数实例超过 4096 个");
        auto index = module.functions.size();
        addConcrete(prepare(specializeGeneric(*callables[declaration].generic, key.arguments)));
        specializations.emplace(std::move(key), index);
        return index;
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
    Symbols& symbols;
    std::vector<Names> scopes;
    std::span<const Type> variables{};
    unsigned loops = 0;
    struct WorkerScope { ExprId id; std::size_t firstLocal; std::optional<Type> result; };
    std::vector<WorkerScope> workers{};
    std::vector<std::optional<ExprId>> catchOwners{};

    std::optional<ExprId> currentWorker() const { return workers.empty() ? std::nullopt : std::optional(workers.back().id); }
    std::optional<Type> returnType() const { return workers.empty() ? std::optional(fn.result) : workers.back().result; }

    void declare(const Identifier& name, std::size_t slot) {
        if (!scopes.back().emplace(name.text, slot).second) throw at(source, name.span, "LYN0002", "名称已在此作用域声明：" + name.text);
    }

    void run() {
        scopes.emplace_back();
        for (std::size_t index = 0; index < fn.syntax.parameters.size(); ++index) declare(fn.syntax.parameters[index].name, index);
        if (fn.syntax.foreignSymbol) return;
        visit(fn.syntax.body, fn.result);
    }

    Type visit(ExprId id, std::optional<Type> expected = {}, bool discard = false) {
        auto type = inspect(id, expected, discard);
        return fit(id, type, expected);
    }

    Type inspect(ExprId id, std::optional<Type> hint, bool discard = false) {
        const auto& expr = fn.syntax.expressions[id];
        return fn.expressions[id].type = std::visit([&](const auto& node) { return infer(id, node, hint, discard); }, expr.node);
    }

    Type fit(ExprId id, Type type, std::optional<Type> expected) {
        auto& info = fn.expressions[id];
        if (expected && type != *expected && type != Type::Never) {
            auto site = fn.syntax.expressions[id].span;
            if (expected->kind != Type::Option || module.sumTypes[expected->id].success != type) {
                info.conversion = directConversion(symbols.conversions, module, symbols.sources, type, *expected, source, site);
                if (!info.conversion) throw mismatch(module, source, site, *expected, type);
            }
            info.sourceType = type;
            type = *expected;
        }
        return info.type = type;
    }

    Type infer(ExprId, std::int64_t, auto, bool) { return Type::Int; }
    Type infer(ExprId, bool, auto, bool) { return Type::Bool; }
    Type infer(ExprId, const StringLiteral&, auto, bool) { return Type::String; }

    Type infer(ExprId, const Raise& raise, auto, bool) {
        if (raise.value) visit(*raise.value, Type::Exception);
        else if (catchOwners.empty() || catchOwners.back() != currentWorker()) throw at(source, raise.site, "LYE0001", "单独的 throw 只可在当前执行体的 catch 中重抛");
        return Type::Never;
    }

    Type infer(ExprId id, const Attempt& attempt, std::optional<Type> expected, bool discard) {
        auto normal = visit(attempt.body, discard ? std::nullopt : expected, discard);
        scopes.emplace_back();
        if (const auto& binding = attempt.binding) {
            if (symbols.resolve(fn.syntax, binding->type, variables) != Type::Exception) throw at(source, binding->type.span, "LYE0002", "当前仅支持捕获普通异常根类型 Exception");
            auto slot = fn.locals.size();
            declare(binding->name, slot);
            fn.locals.push_back({Type::Exception, false, currentWorker()});
            fn.expressions[id].target = slot;
        }
        catchOwners.push_back(currentWorker());
        auto recovered = visit(attempt.handler, discard ? std::nullopt : normal != Type::Never ? std::optional(normal) : expected, discard);
        catchOwners.pop_back();
        scopes.pop_back();
        if (normal == Type::Never && recovered == Type::Never) return Type::Never;
        return discard ? Type{Type::Unit} : normal == Type::Never ? recovered : normal;
    }

    Type infer(ExprId id, const ListLiteral& literal, std::optional<Type> expected, bool) {
        if (expected && expected->kind == Type::Option) expected = module.sumTypes[expected->id].success;
        std::optional<Type> element;
        if (expected && expected->kind == Type::List) element = module.listElements[expected->id];
        bool exits = false;
        for (auto item : literal.elements) {
            auto value = visit(item, element);
            if (value == Type::Never) exits = true;
            else if (!element) element = value;
        }
        if (exits) return Type::Never;
        if (!element) throw at(source, fn.syntax.expressions[id].span, "LYT0014", "空列表需要明确的 List<T> 目标类型");
        return symbols.list(*element);
    }

    Type infer(ExprId, const Index& index, auto, bool) {
        auto list = visit(index.receiver);
        auto offset = visit(index.offset, Type::Int);
        if (list == Type::Never || offset == Type::Never) return Type::Never;
        if (list.kind != Type::List) throw at(source, index.site, "LYT0014", "下标需要 List，实际是 " + typeName(module, list));
        return module.listElements[list.id];
    }

    Type infer(ExprId id, const EmptyOption&, std::optional<Type> expected, bool) {
        if (!expected || expected->kind != Type::Option) throw at(source, fn.syntax.expressions[id].span, "LYT0010", "None/null 需要明确的 Option 目标类型");
        return *expected;
    }

    Type infer(ExprId id, const Propagate& propagation, std::optional<Type> expected, bool) {
        std::optional<Type> hint;
        auto target = returnType();
        if (!target) throw at(source, propagation.site, "LYT0013", "请给 blocking 块明确的 Task<Option<T>> 或 Task<Result<T,E>> 目标类型以使用 ?");
        if (expected && (target->kind == Type::Option || target->kind == Type::Result)) hint = symbols.sum(target->kind, *expected, module.sumTypes[target->id].failure);
        auto operand = inspect(propagation.operand, hint);
        if (operand == Type::Never) return operand;
        if (operand.kind != Type::Option && operand.kind != Type::Result) throw at(source, propagation.site, "LYT0013", "? 需要 Option 或 Result，实际是 " + typeName(module, operand));
        if (operand.kind == Type::Result && target->kind == Type::Result && module.sumTypes[target->id].failure != module.sumTypes[operand.id].failure) {
            fn.expressions[id].errorConversion = directConversion(symbols.conversions, module, symbols.sources, module.sumTypes[operand.id].failure, module.sumTypes[target->id].failure, source, propagation.site);
            if (fn.expressions[id].errorConversion) return module.sumTypes[operand.id].success;
        }
        if (target->kind != operand.kind || (operand.kind == Type::Result && module.sumTypes[target->id].failure != module.sumTypes[operand.id].failure)) {
            throw at(source, propagation.site, "LYT0013", "? 无法将 " + typeName(module, operand) + " 的失败传播到 " + typeName(module, *target));
        }
        return module.sumTypes[operand.id].success;
    }

    bool untypedOption(ExprId id) const {
        const auto& node = fn.syntax.expressions[id].node;
        if (std::holds_alternative<EmptyOption>(node)) return true;
        const auto* call = std::get_if<Call>(&node);
        return call && !call->receiver && call->name.text == "Some" && call->arguments.size() == 1 && untypedOption(call->arguments.front().value);
    }

    Type infer(ExprId id, const Match& match, std::optional<Type> expected, bool discard) {
        std::optional<Type> subjectHint, fallback;
        if (match.optional == OptionalForm::Default && untypedOption(match.subject)) {
            auto element = expected;
            if (!element) {
                fallback = visit(match.arms.back().value);
                element = *fallback == Type::Never ? Type{Type::Unit} : *fallback;
            }
            subjectHint = symbols.sum(Type::Option, *element);
        }
        auto subject = inspect(match.subject, subjectHint);
        if (subject == Type::Never) return subject;
        if (match.optional != OptionalForm::None && subject.kind != Type::Option)
            throw at(source, match.operation, "LYT0010", "可空运算符需要 Option，实际是 " + typeName(module, subject));
        if (subject.kind != Type::Option && subject.kind != Type::Result) throw at(source, fn.syntax.expressions[match.subject].span, "LYT0011", "match 需要 Option 或 Result，实际是 " + typeName(module, subject));
        bool discardArms = discard && match.optional == OptionalForm::None;
        std::vector<const SumPattern*> patterns;
        std::optional<Type> result;
        for (const auto& arm : match.arms) {
            auto payload = subject;
            for (auto [tag, span] : arm.pattern.tags) {
                if (payload.kind != (optionTag(tag) ? Type::Option : Type::Result)) throw at(source, span, "LYT0011", std::string(tagName(tag)) + " 不能匹配 " + typeName(module, payload));
                const auto& fields = module.sumTypes[payload.id];
                payload = successTag(tag) ? fields.success : fields.failure;
            }
            scopes.emplace_back();
            auto slot = fn.locals.size();
            if (const auto& binding = arm.pattern.binding) {
                declare({binding->name, binding->span}, slot);
                fn.locals.push_back({payload, false, currentWorker()});
            }
            fn.expressions[id].argumentSlots.push_back(slot);
            auto hint = result ? result : expected;
            Type value;
            if (match.optional == OptionalForm::Access && &arm == &match.arms.front()) {
                value = visit(arm.value);
                if (value != Type::Never && value.kind != Type::Option) value = fit(arm.value, value, symbols.sum(Type::Option, value));
            } else if (match.optional == OptionalForm::Access) {
                if (!hint || hint->kind != Type::Option) hint = symbols.sum(Type::Option, Type::Unit);
                value = visit(arm.value, hint);
            } else if (fallback && &arm == &match.arms.back()) value = *fallback;
            else value = visit(arm.value, discardArms ? std::nullopt : hint, discardArms);
            if (value != Type::Never && !result) result = discardArms ? Type::Unit : value;
            scopes.pop_back();
            patterns.push_back(&arm.pattern);
        }
        auto cases = [&](Type type) -> std::optional<std::array<SumCase<Type>, 2>> {
            if (type.kind != Type::Result && type.kind != Type::Option) return std::nullopt;
            const auto& fields = module.sumTypes[type.id];
            if (type.kind == Type::Option) return std::array{SumCase<Type>{SumTag::None, Type::Unit}, SumCase<Type>{SumTag::Some, fields.success}};
            return std::array{SumCase<Type>{SumTag::Ok, fields.success}, SumCase<Type>{SumTag::Err, fields.failure}};
        };
        if (auto missing = missingPattern(subject, patterns, cases)) throw at(source, fn.syntax.expressions[id].span, "LYT0012", "match 未穷尽，缺少分支：" + *missing);
        return discard && match.optional != OptionalForm::None ? Type{Type::Unit} : result.value_or(Type::Never);
    }

    Type infer(ExprId id, const Identifier& name, auto, bool) {
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
            if (auto found = scope->find(name.text); found != scope->end()) {
                auto slot = found->second;
                for (const auto& worker : workers) if (slot < worker.firstLocal) {
                    fn.locals[slot].captured = true;
                    auto& captures = fn.expressions[worker.id].captures;
                    if (std::ranges::find(captures, slot) == captures.end()) captures.push_back(slot);
                }
                fn.expressions[id].target = slot;
                return fn.locals[slot].type;
            }
        }
        throw at(source, name.span, "LYN0001", "找不到绑定：" + name.text);
    }

    Type infer(ExprId id, const Binding& binding, auto, bool) {
        std::optional<Type> annotation;
        if (binding.annotation) annotation = symbols.resolve(fn.syntax, *binding.annotation, variables);
        auto value = visit(binding.value, annotation);
        auto slot = fn.locals.size();
        declare(binding.name, slot);
        fn.locals.push_back({annotation.value_or(value), binding.writable, currentWorker()});
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
            if (arg.spread) throw at(source, *arg.spread, "LYT0002", "没有可接收展开元素的变长形参");
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

    template<class GetType>
    bool functionArguments(ExprId id, const Call& call, const Function& target, GetType getType) {
        auto skip = static_cast<std::size_t>(call.receiver.has_value());
        std::vector<ParameterShape> parameters;
        for (auto slot = skip; slot < target.parameters.size(); ++slot) {
            const auto& parameter = target.parameters[slot];
            parameters.push_back({parameter.name.text, parameter.defaultValue.has_value(), parameter.variadic});
        }
        std::vector<ArgumentShape> arguments;
        for (const auto& argument : call.arguments) {
            std::optional<std::string_view> name;
            auto span = argument.spread.value_or(fn.syntax.expressions[argument.value].span);
            if (argument.label) { name = argument.label->text; span = argument.label->span; }
            arguments.push_back({name, argument.spread.has_value(), span});
        }
        auto plan = bindArguments(parameters, arguments, source, call.name.span, call.name.text);
        auto& info = fn.expressions[id];
        for (auto slot : plan.defaults) info.omittedDefaults.push_back(slot + skip);
        bool exits = false;
        for (std::size_t index = 0; index < call.arguments.size(); ++index) {
            auto selected = plan.explicitSlots[index];
            info.argumentSlots.push_back(selected.index);
            std::optional<Type> type = getType(selected.index + skip);
            if (type && selected.mode == ArgumentMode::Spread) type = symbols.list(*type);
            auto expr = call.arguments[index].value;
            auto inferred = inspect(expr, type);
            if (selected.mode == ArgumentMode::Item && type && inferred.kind == Type::List && module.listElements[inferred.id] == *type) {
                auto error = mismatch(module, source, arguments[index].site, *type, inferred);
                error.hint = "若要逐项传入列表，请在实参前写 ...。";
                throw error;
            }
            auto value = fit(expr, inferred, type);
            if (value == Type::Never) exits = true;
            else if (selected.mode == ArgumentMode::Spread && value.kind != Type::List) throw at(source, arguments[index].site, "LYT0001", "展开需要 List，实际是 " + typeName(module, value));
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
        if (owner.kind == Type::List && member.name.text == "size") return Type::Int;
        if (member.iterationExtent) throw at(source, member.name.span, "LYT0014", "for ... in 需要 List，实际是 " + typeName(module, owner));
        if (owner == Type::Exception && member.name.text == "message") return Type::String;
        auto field = memberIndex(owner, member.name, symbols.fields);
        fn.expressions[id].target = field;
        const auto& record = module.aggregates[owner.id];
        visible(record.syntax.file, record.syntax.fields[field].isPublic, member.name);
        return record.fields[field];
    }

    Type genericCall(std::size_t declaration, ExprId id, const Call& call, std::optional<Type> expected) {
        const auto& target = *symbols.callables[declaration].generic;
        std::vector<Type> explicitTypes;
        for (const auto& type : call.typeArguments) explicitTypes.push_back(symbols.resolve(fn.syntax, type, variables));
        if (!explicitTypes.empty() && explicitTypes.size() != target.typeParameters.size()) {
            throw at(source, call.name.span, "LYG0001", call.name.text + " 需要 " + std::to_string(target.typeParameters.size()) + " 个类型实参");
        }
        auto skip = static_cast<std::size_t>(call.receiver.has_value());
        bool exits = functionArguments(id, call, target, [&](auto slot) -> std::optional<Type> {
                    const auto& annotation = target.parameters[slot].type;
                    if (genericPattern(target, typeForm(annotation.text))) return explicitTypes.empty() ? std::nullopt : std::optional(symbols.resolve(target, annotation, explicitTypes));
                    return symbols.resolve(symbols.sources[target.file], annotation);
                });
        auto& info = fn.expressions[id];
        std::vector<GenericArgument<Type>> actuals;
        for (std::size_t index = 0; index < call.arguments.size(); ++index) {
            auto expr = call.arguments[index].value;
            auto type = fn.expressions[expr].type;
            if (call.arguments[index].spread && type != Type::Never) type = module.listElements[type.id];
            actuals.push_back({info.argumentSlots[index] + skip, type, fn.syntax.expressions[expr].span});
        }
        if (target.isAsync && expected) expected = expected->kind == Type::Task ? std::optional(module.taskResults[expected->id]) : std::nullopt;
        auto types = inferGenericArguments<Type>(target, call, fn.syntax.expressions[id].span, explicitTypes, actuals, expected, source, [&](Type type) { return typeName(module, type); },
                [&](Type value, const std::string& name, std::size_t slot) { return symbols.project(value, name, slot); });
        for (std::size_t index = 0; index < actuals.size(); ++index) {
            const auto& argument = actuals[index];
            auto type = symbols.resolve(target, target.parameters[argument.slot].type, types);
            auto expr = call.arguments[index].value;
            fit(expr, fn.expressions[expr].type, call.arguments[index].spread ? symbols.list(type) : type);
        }
        if (variables.empty()) info.target = symbols.instantiate(declaration, types, source, call.name.span);
        auto result = symbols.resolve(target, target.result, types);
        return exits ? Type{Type::Never} : target.isAsync ? symbols.task(result) : result;
    }

    Type infer(ExprId id, const Call& call, std::optional<Type> expected, bool) {
        auto& info = fn.expressions[id];
        auto noTypeArgs = [&] { if (!call.typeArguments.empty()) throw at(source, call.typeArguments.front().span, "LYG0001", "此调用不接受类型实参：" + call.name.text); };
        std::size_t declaration;
        if (call.receiver) {
            auto owner = visit(*call.receiver);
            if (owner == Type::Never) return owner;
            if (owner.kind == Type::Task && call.name.text == "cancel") {
                if (!workers.empty() || !fn.syntax.isAsync) throw at(source, call.name.span, "LYT0008", "Task.cancel() 只能在 async 函数中调用，不能在 blocking 或 defer 中调用");
                noTypeArgs();
                if (!call.arguments.empty()) throw at(source, call.name.span, "LYT0002", "Task.cancel() 不接收实参");
                info.callee = Callee::TaskCancel;
                return Type::Unit;
            }
            if (owner.kind == Type::List && call.name.text == "add") {
                noTypeArgs();
                info.callee = Callee::ListAdd;
                return arguments(id, call, 1, [](auto) { return "value"; }, [&](auto) { return module.listElements[owner.id]; }) ? Type::Never : Type::Unit;
            }
            declaration = memberIndex(owner, call.name, symbols.methods);
            const auto& item = symbols.callables[declaration];
            const auto& method = item.generic ? *item.generic : module.functions[item.target].syntax;
            visible(method.file, method.isPublic, call.name);
        } else if (auto tag = sumTag(call.name.text); tag && *tag != SumTag::None) {
            noTypeArgs();
            if (call.arguments.size() != 1 || call.arguments[0].label || call.arguments[0].spread) throw at(source, call.name.span, "LYT0002", call.name.text + " 需要一个位置实参");
            auto kind = optionTag(*tag) ? Type::Option : Type::Result;
            if (expected && expected->kind == Type::Option && kind == Type::Result) expected = module.sumTypes[expected->id].success;
            if (expected && expected->kind != kind) expected.reset();
            if (!expected && kind == Type::Result) throw at(source, call.name.span, "LYT0010", call.name.text + " 需要明确的 Result 目标类型");
            std::optional<Type> payload;
            if (expected) payload = successTag(*tag) ? module.sumTypes[expected->id].success : module.sumTypes[expected->id].failure;
            auto value = visit(call.arguments[0].value, payload);
            info.callee = Callee::Sum;
            info.target = static_cast<std::size_t>(*tag);
            info.argumentSlots.push_back(0);
            if (value == Type::Never) return value;
            return expected ? *expected : symbols.sum(kind, value);
        } else if (call.name.text == "Exception") {
            noTypeArgs();
            info.callee = Callee::Exception;
            return arguments(id, call, 1, [](auto) { return "message"; }, [](auto) { return Type{Type::String}; }) ? Type::Never : Type::Exception;
        } else if (call.name.text == "print") {
            noTypeArgs();
            info.callee = Callee::Print;
            bool exits = arguments(id, call, 1, [](auto) { return "value"; }, [](auto) { return std::nullopt; });
            auto arg = call.arguments.front().value;
            auto type = fn.expressions[arg].type;
            if (!exits && type != Type::Int && type != Type::String) throw at(source, fn.syntax.expressions[arg].span, "LYT0001", "print 需要 Int 或 String，实际是 " + typeName(module, type));
            return exits ? Type::Never : Type::Unit;
        } else if (call.name.text == "gcCollect") {
            noTypeArgs();
            info.callee = Callee::Collect;
            if (!call.arguments.empty()) throw at(source, call.name.span, "LYT0002", "gcCollect 不接收实参");
            return Type::Unit;
        } else if (auto record = symbols.records.find(call.name.text); record != symbols.records.end()) {
            noTypeArgs();
            info.callee = Callee::Constructor;
            info.target = record->second;
            const auto& decl = module.aggregates[info.target];
            for (const auto& field : decl.syntax.fields) visible(decl.syntax.file, field.isPublic, call.name);
            bool exits = arguments(id, call, decl.fields.size(), [&](auto slot) -> const auto& { return decl.syntax.fields[slot].name.text; }, [&](auto slot) { return decl.fields[slot]; });
            return exits ? Type{Type::Never} : Type{Type::Record, info.target};
        } else {
            auto found = symbols.functions.find(call.name.text);
            if (found == symbols.functions.end()) throw at(source, call.name.span, "LYN0001", "找不到函数：" + call.name.text);
            declaration = found->second;
        }
        if (symbols.callables[declaration].generic) return genericCall(declaration, id, call, expected);
        noTypeArgs();
        info.target = symbols.callables[declaration].target;
        const auto& target = module.functions[info.target];
        bool exits = functionArguments(id, call, target.syntax, [&](auto slot) {
            auto type = target.locals[slot].type;
            return target.syntax.parameters[slot].variadic ? module.listElements[type.id] : type;
        });
        return exits ? Type{Type::Never} : target.syntax.isAsync ? symbols.task(target.result) : target.result;
    }

    Type infer(ExprId, const TaskOp& op, std::optional<Type> expected, bool) {
        if (!workers.empty() || !fn.syntax.isAsync) throw at(source, op.site, "LYT0008", "await 和 spawn 只能在 async 函数中使用，不能用于同步的 blocking 或 defer");
        auto operand = inspect(op.operand, op.spawn || !expected ? expected : std::optional(symbols.task(*expected)));
        if (operand.kind == Type::Never) return operand;
        if (operand.kind != Type::Task) throw at(source, fn.syntax.expressions[op.operand].span, "LYT0008", "此处需要 Task，实际是 " + typeName(module, operand));
        return op.spawn ? operand : module.taskResults[operand.id];
    }

    Type infer(ExprId id, const Blocking& worker, std::optional<Type> expected, bool) {
        auto hint = expected && expected->kind == Type::Task ? std::optional(module.taskResults[expected->id]) : std::nullopt;
        workers.push_back({id, fn.locals.size(), hint});
        unsigned previous = std::exchange(loops, 0);
        auto body = visit(worker.body, hint);
        auto result = workers.back().result.value_or(body);
        fit(worker.body, body, result);
        workers.pop_back();
        loops = previous;
        return symbols.task(result);
    }

    Type infer(ExprId id, const Deferred& cleanup, auto, bool) {
        auto owner = currentWorker();
        if (std::ranges::find(fn.cleanupOwners, owner) == fn.cleanupOwners.end()) fn.cleanupOwners.push_back(owner);
        workers.push_back({id, fn.locals.size(), Type{Type::Unit}});
        unsigned previous = std::exchange(loops, 0);
        visit(cleanup.body, {}, true);
        loops = previous;
        workers.pop_back();
        return Type::Unit;
    }

    bool writable(ExprId id) const {
        const auto& expr = fn.syntax.expressions[id];
        if (std::holds_alternative<Index>(expr.node)) return true;
        if (std::holds_alternative<Identifier>(expr.node)) return fn.locals[fn.expressions[id].target].writable;
        if (auto member = std::get_if<Member>(&expr.node)) {
            auto type = fn.expressions[member->receiver].type;
            if (type.kind != Type::Record) return false;
            const auto& owner = module.aggregates[type.id].syntax;
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
        bool emptyLeft = std::holds_alternative<EmptyOption>(fn.syntax.expressions[binary.left].node);
        bool emptyRight = std::holds_alternative<EmptyOption>(fn.syntax.expressions[binary.right].node);
        if (equality && (emptyLeft || emptyRight)) {
            auto other = visit(emptyLeft ? binary.right : binary.left);
            if (other == Type::Never) return other;
            visit(emptyLeft ? binary.left : binary.right, other);
            return Type::Bool;
        }
        auto left = visit(binary.left, equality || binary.op == Op::Add ? std::nullopt : std::optional(Type::Int));
        auto right = visit(binary.right, left == Type::Never ? std::nullopt : std::optional(left));
        if (left == Type::Never || right == Type::Never) return Type::Never;
        if (equality && (left.kind == Type::Option || left.kind == Type::Result || left.kind == Type::Variable || (left.kind == Type::Record && !module.aggregates[left.id].syntax.reference))) throw at(source, binary.site, "LYT0006", typeName(module, left) + " 未提供 Eq，不能用 == 或 != 比较");
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
        auto target = returnType();
        auto value = ret.value ? visit(*ret.value, target) : Type{Type::Unit};
        if (target && value != Type::Never && *target != value) throw mismatch(module, source, fn.syntax.expressions[id].span, *target, value);
        if (!target && value != Type::Never) workers.back().result = value;
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
    if (!module.expansions.empty()) {
        auto& use = module.expansions.front();
        throw at(sources[use.file], use.name.span, "LYM0001", "声明前缀未展开：" + use.name.text);
    }
    CheckedModule checked{};
    Symbols symbols{checked, sources, {}, {}, {}, {}, {}, {}};
    for (auto& decl : module.aggregates) {
        auto id = checked.aggregates.size();
        for (Type builtin : {Type::Int, Type::Bool, Type::String, Type::Unit, Type::Never, Type::CPtr, Type::Exception}) {
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
            auto descend = [&](auto&& self, Type field) -> void {
                if (field.kind == Type::Option || field.kind == Type::Result) {
                    self(self, checked.sumTypes[field.id].success);
                    if (field.kind == Type::Result) self(self, checked.sumTypes[field.id].failure);
                } else if (field.kind == Type::Record && !checked.aggregates[field.id].syntax.reference) {
                    if (marks[field.id] == Mark::Visiting) throw at(sources[record.syntax.file], record.syntax.fields[slot].type.span, "LYT0007", "值字段形成无限大小的内联布局：" + record.syntax.name.text + " → " + typeName(checked, field));
                    visit(visit, field.id);
                }
            };
            descend(descend, record.fields[slot]);
        }
        marks[id] = Mark::Done;
    };
    for (std::size_t id = 0; id < checked.aggregates.size(); ++id) layout(layout, id);
    for (auto& function : module.functions) {
        const auto& source = sources[function.file];
        auto owner = function.owner ? std::optional(symbols.resolve(source, *function.owner).id) : std::nullopt;
        auto& names = owner ? symbols.methods[*owner] : symbols.functions;
        bool collision = owner ? symbols.fields[*owner].contains(function.name.text) : symbols.records.contains(function.name.text) || function.name.text == "print" || function.name.text == "gcCollect";
        if (collision || !names.emplace(function.name.text, symbols.callables.size()).second) {
            throw at(source, function.name.span, "LYN0002", "函数名已声明：" + function.name.text);
        }
        if (!function.typeParameters.empty()) {
            if (!owner && requireEntry && function.name.text == "main") throw at(source, function.name.span, "LYG0001", "main 不能声明类型形参");
            std::set<std::string> seen; std::vector<Type> variables;
            for (const auto& parameter : function.typeParameters) {
                if (!seen.insert(parameter.text).second) throw at(source, parameter.span, "LYN0002", "类型形参重复：" + parameter.text);
                variables.emplace_back(Type::Variable, checked.variableNames.size());
                checked.variableNames.push_back(parameter.text);
            }
            symbols.callables.push_back({0, std::move(function), std::move(variables)});
            continue;
        }
        auto concrete = symbols.prepare(std::move(function));
        const auto& syntax = concrete.syntax;
        if (!owner && requireEntry && syntax.name.text == "main") {
            if (!syntax.parameters.empty()) throw at(source, syntax.name.span, "LYT0002", "main 不能接收形参");
            if (syntax.isAsync && concrete.result != Type::Unit) {
                auto expected = symbols.sum(Type::Result, Type::Unit, Type::String);
                if (concrete.result != expected) throw mismatch(checked, source, syntax.result.span, expected, concrete.result);
            } else if (concrete.result != Type::Unit) throw mismatch(checked, source, syntax.result.span, Type::Unit, concrete.result);
            checked.entry = checked.functions.size();
        }
        symbols.callables.push_back({checked.functions.size(), {}, {}});
        symbols.addConcrete(std::move(concrete));
    }
    for (std::size_t index = 0; index < checked.functions.size(); ++index) if (checked.functions[index].syntax.conversion) registerConversion(symbols.conversions, checked, sources, index);
    for (const auto& callable : symbols.callables) if (callable.generic) {
        for (std::size_t slot = 0; slot < callable.generic->parameters.size(); ++slot) if (callable.generic->parameters[slot].defaultValue) {
            auto helper = symbols.prepare(defaultFunction(*callable.generic, slot), callable.variables);
            Checker{helper, sources[helper.syntax.file], checked, symbols, {}, callable.variables}.run();
        }
        auto body = symbols.prepare(*callable.generic, callable.variables);
        Checker{body, sources[body.syntax.file], checked, symbols, {}, callable.variables}.run();
    }
    for (std::size_t index = 0; index < checked.functions.size(); ++index) {
        auto& fn = checked.functions[index];
        Checker{fn, sources[fn.syntax.file], checked, symbols, {}}.run();
    }
    validateForeignDeclarations(checked, sources);
    if (requireEntry && !checked.entry) throw Diagnostic{"LYN0001", "可执行包缺少入口 fn main(): Unit"};
    return checked;
}

}
