module;
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

module lyra.check;
import lyra.generics;
import lyra.foreign;
import lyra.conversions;
import lyra.traits;
import lyra.collections;
import lyra.patterns;
import lyra.numbers;
import lyra.initialization;

namespace lyra {
namespace {

using Names = std::unordered_map<std::string, std::size_t>;

const std::string& linkedName(const Identifier& name) { return name.linked.empty() ? name.text : name.linked; }

constexpr Type scalarTypes[] = {Type::Bool, Type::String, Type::Char, Type::Bytes, Type::Unit, Type::CPtr, Type::Exception};

bool builtInDisplay(Type type) { return numericType(type) || type == Type::Bool || type == Type::String || type == Type::Char; }

std::string typeName(const CheckedModule& module, Type type, bool canonical = false) {
    auto name = [&](Type nested) { return typeName(module, nested, canonical); };
    switch (type.kind) {
        case Type::Bool: return "Bool";
        case Type::Int: case Type::UInt: case Type::Real: return numberName(type);
        case Type::Unit: return "Unit";
        case Type::Never: return "Never";
        case Type::String: return "String";
        case Type::Char: return "Char";
        case Type::Bytes: return "Bytes";
        case Type::Exception: return "Exception";
        case Type::CPtr: return "CPtr";
        case Type::Record: {
            const auto& record = module.aggregates[type.id];
            const auto& full = record.syntax.name.text;
            auto separator = full.rfind("::");
            auto text = canonical || separator == full.npos ? full : full.substr(separator + 2);
            if (record.arguments.empty()) return text;
            text += '<';
            for (std::size_t index = 0; index < record.arguments.size(); ++index) {
                if (index) text += canonical ? "," : ", ";
                text += name(record.arguments[index]);
            }
            return text + '>';
        }
        case Type::Enum: {
            const auto& full = module.enums[type.id].syntax.name.text;
            auto separator = full.rfind("::");
            return canonical || separator == full.npos ? full : full.substr(separator + 2);
        }
        case Type::Variable: return module.variableNames[type.id];
        case Type::Task: return "Task<" + name(module.taskResults[type.id]) + ">";
        case Type::List: return "List<" + name(module.listElements[type.id]) + ">";
        case Type::Option: return "Option<" + name(module.sumTypes[type.id].success) + ">";
        case Type::Result: return "Result<" + name(module.sumTypes[type.id].success) + "," + name(module.sumTypes[type.id].failure) + ">";
        case Type::Closure: {
            const auto& shape = module.signatures[type.id];
            std::string out = canonical ? "$fn<" : "(";
            for (std::size_t slot = 0; slot < shape.parameters.size(); ++slot) {
                if (slot) out += canonical ? "," : ", ";
                out += name(shape.parameters[slot]);
            }
            if (canonical) return out + (shape.parameters.empty() ? "" : ",") + name(shape.result) + ">";
            return out + ") -> " + name(shape.result);
        }
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
    syntax.initializer = false;
    syntax.parameters.resize(slot);
    for (auto& earlier : syntax.parameters) earlier.defaultValue.reset();
    return syntax;
}

Function constructorSignature(const Aggregate& record) {
    auto result = record.name;
    result.text += '<';
    for (std::size_t index = 0; index < record.typeParameters.size(); ++index) {
        if (index) result.text += ',';
        result.text += record.typeParameters[index].text;
    }
    result.text += '>';
    std::vector<Parameter> parameters;
    for (const auto& field : record.fields) parameters.push_back({field.name, field.type});
    return {record.file, record.name, std::move(parameters), std::move(result), {}, 0, {}, record.isPublic, record.typeParameters};
}

struct Symbols {
    CheckedModule& module;
    const std::vector<Source>& sources;
    Names records, functions;
    std::vector<Names> fields, methods;
    std::vector<Callable> callables;
    std::map<GenericInstanceKey, std::size_t> specializations;
    std::vector<DirectConversion> conversions{};
    StructuralContracts contracts{};
    Names enums{};
    std::vector<Names> variants{};
    std::vector<Aggregate> recordDeclarations{};
    std::vector<std::optional<std::size_t>> initializers{};
    std::vector<Names> staticMethods{};
    mutable std::map<std::pair<std::size_t, std::vector<Type>>, Type> recordInstances{};
    mutable unsigned expandingRecord{};

    const Function& declaration(const Callable& callable) const { return callable.generic ? *callable.generic : module.functions[callable.target].syntax; }

    std::optional<RequiredMethod> methodShape(Type receiver, const Callable& candidate, const Source& use) const {
        const auto& record = module.aggregates[receiver.id];
        const auto& actual = declaration(candidate);
        if ((!actual.isPublic && sources[actual.file].moduleId != use.moduleId) || actual.typeParameters.size() != record.arguments.size()) return {};
        auto type = [&](const Identifier& annotation) { return resolve(actual, annotation, record.arguments); };
        RequiredMethod result{{}, {}, type(actual.result), actual.isAsync};
        for (std::size_t slot = 1; slot < actual.parameters.size(); ++slot) {
            const auto& parameter = actual.parameters[slot];
            if (parameter.variadic) return {};
            result.labels.push_back(parameter.name.text);
            result.parameters.push_back(type(parameter.type));
        }
        return result;
    }

    void requireBounds(const Function& target, std::span<const Type> types, const Source& use, Span site) const {
        contracts.require(target, types, use, site, [&](Type receiver, const std::string& name) -> std::optional<RequiredMethod> {
            if (receiver.kind != Type::Record) return {};
            const auto& members = methods[module.aggregates[receiver.id].declaration];
            auto found = members.find(name);
            return found == members.end() ? std::nullopt : methodShape(receiver, callables[found->second], sources[target.file]);
        }, [&](Type type) { return typeName(module, type); });
    }

    Type unaryType(Type::Kind kind, std::vector<Type>& types, Type result) const {
        auto found = std::find(types.begin(), types.end(), result);
        auto index = static_cast<std::size_t>(found - types.begin());
        if (found == types.end()) types.push_back(result);
        return {kind, index};
    }

    Type task(Type result) const { return unaryType(Type::Task, module.taskResults, result); }
    Type list(Type element) const { return unaryType(Type::List, module.listElements, element); }

    Type closure(std::vector<Type> parameters, Type result) const {
        FunctionShape shape{std::move(parameters), result};
        auto& types = module.signatures;
        auto found = std::ranges::find(types, shape);
        auto index = static_cast<std::size_t>(found - types.begin());
        if (found == types.end()) types.push_back(std::move(shape));
        return {Type::Closure, index};
    }

    Type sum(Type::Kind kind, Type success, Type failure = Type::Unit) const {
        auto& types = module.sumTypes;
        SumTypes fields{success, failure};
        auto found = std::find(types.begin(), types.end(), fields);
        auto index = static_cast<std::size_t>(found - types.begin());
        if (found == types.end()) types.push_back(fields);
        return {kind, index};
    }

    std::optional<Type> project(Type value, const std::string& name, std::size_t slot) const {
        if (value.kind == Type::Record) {
            const auto& record = module.aggregates[value.id];
            if (record.syntax.name.text == name && slot < record.arguments.size()) return record.arguments[slot];
        }
        if (name == "$fn" && value.kind == Type::Closure) {
            const auto& shape = module.signatures[value.id];
            if (slot < shape.parameters.size()) return shape.parameters[slot];
            if (slot == shape.parameters.size()) return shape.result;
        }
        if (name == "Task" && value.kind == Type::Task && slot == 0) return module.taskResults[value.id];
        if (name == "List" && value.kind == Type::List && slot == 0) return module.listElements[value.id];
        if (name == "Option" && value.kind == Type::Option && slot == 0) return module.sumTypes[value.id].success;
        if (name == "Result" && value.kind == Type::Result && slot < 2) return slot ? module.sumTypes[value.id].failure : module.sumTypes[value.id].success;
        return std::nullopt;
    }

    bool abstract(Type type) const {
        switch (type.kind) {
            case Type::Variable: return true;
            case Type::Record: return module.aggregates[type.id].abstract;
            case Type::Task: return abstract(module.taskResults[type.id]);
            case Type::List: return abstract(module.listElements[type.id]);
            case Type::Option: return abstract(module.sumTypes[type.id].success);
            case Type::Result: return abstract(module.sumTypes[type.id].success) || abstract(module.sumTypes[type.id].failure);
            case Type::Closure: {
                const auto& shape = module.signatures[type.id];
                return abstract(shape.result) || std::ranges::any_of(shape.parameters, [&](Type parameter) { return abstract(parameter); });
            }
            default: return false;
        }
    }

    Type recordType(std::size_t declaration, std::vector<Type> arguments, const Source& source, Span span) const {
        const auto& syntax = recordDeclarations[declaration];
        if (arguments.size() != syntax.typeParameters.size())
            throw at(source, span, "LYG0001", syntax.name.text + " 需要 " + std::to_string(syntax.typeParameters.size()) + " 个类型实参");
        if (auto found = recordInstances.find({declaration, arguments}); found != recordInstances.end()) return found->second;
        if (expandingRecord >= 128 || recordInstances.size() >= 4096)
            throw at(source, span, "LYG0004", "记录类型实例展开过深或超出编译单元实例预算：" + syntax.name.text);
        Type type{Type::Record, module.aggregates.size()};
        bool symbolic = std::ranges::any_of(arguments, [&](Type argument) { return abstract(argument); });
        recordInstances.emplace(std::pair{declaration, arguments}, type);
        auto& record = module.aggregates.emplace_back(syntax, std::vector<Type>{}, declaration, std::move(arguments), symbolic);
        ++expandingRecord;
        for (const auto& field : syntax.fields) {
            auto resolved = resolveWith(typeForm(field.type.text), sources[syntax.file], field.type.span, syntax.typeParameters,
                [&](std::size_t slot) { return record.arguments[slot]; });
            record.fields.push_back(resolved.value());
        }
        --expandingRecord;
        return type;
    }

    template<class Lookup>
    std::optional<Type> resolveWith(const TypeForm& form, const Source& source, Span span, std::span<const Identifier> parameters, const Lookup& lookup) const {
        if (form.arguments.empty()) {
            auto found = std::ranges::find(parameters, form.name, &Identifier::text);
            if (found != parameters.end()) return lookup(static_cast<std::size_t>(found - parameters.begin()));
            if (auto type = numberType(form.name)) return type;
            for (Type type : scalarTypes) if (form.name == typeName(module, type)) return type;
            if (auto item = enums.find(form.name); item != enums.end()) return Type{Type::Enum, item->second};
        }
        std::vector<Type> arguments;
        for (const auto& part : form.arguments) {
            auto value = resolveWith(part, source, span, parameters, lookup);
            if (!value) return std::nullopt;
            arguments.push_back(*value);
        }
        if (form.name == "$fn") {
            auto result = arguments.back();
            arguments.pop_back();
            return closure(std::move(arguments), result);
        }
        if (arguments.size() == 1 && form.name == "Task") return task(arguments[0]);
        if (arguments.size() == 1 && form.name == "List") return list(arguments[0]);
        if (arguments.size() == 1 && form.name == "Option") return sum(Type::Option, arguments[0]);
        if (arguments.size() == 2 && form.name == "Result") return sum(Type::Result, arguments[0], arguments[1]);
        if (auto record = records.find(form.name); record != records.end()) return recordType(record->second, std::move(arguments), source, span);
        throw at(source, span, "LYT0003", "找不到类型或类型实参数量不符：" + form.name);
    }

    Type resolveForm(const TypeForm& form, const Source& source, Span span, const Function* signature, std::span<const Type> types) const {
        return resolveWith(form, source, span, signature ? std::span<const Identifier>(signature->typeParameters) : std::span<const Identifier>{}, [&](std::size_t slot) { return types[slot]; }).value();
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
        for (auto type : arguments) key.arguments.push_back(typeName(module, type, true));
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
    std::map<ExprId, std::vector<std::optional<Type>>> lambdaHints{};
    std::optional<std::size_t> receiverSlot{};
    std::vector<std::optional<ExprId>> implicitReceivers{};

    std::optional<ExprId> currentWorker() const { return workers.empty() ? std::nullopt : std::optional(workers.back().id); }
    std::optional<Type> returnType() const { return workers.empty() ? std::optional(fn.result) : workers.back().result; }

    std::string localKey(const Identifier& name) const {
        auto mark = source.hygieneAt(name.span);
        return mark ? "$macro" + std::to_string(mark) + ":" + name.text : name.text;
    }

    void declare(const Identifier& name, std::size_t slot) {
        if (!scopes.back().emplace(localKey(name), slot).second) throw at(source, name.span, "LYN0002", "名称已在此作用域声明：" + name.text);
    }

    std::optional<std::size_t> lookupLocal(const Identifier& name) const {
        auto key = localKey(name);
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
            if (auto found = scope->find(key); found != scope->end()) return found->second;
        return {};
    }

    bool implicitMember(const Identifier& name, bool call = false) const {
        if (!receiverSlot || source.hygieneAt(name.span)) return false;
        auto owner = module.aggregates[fn.locals[*receiverSlot].type.id].declaration;
        return symbols.fields[owner].contains(name.text) || (call && symbols.methods[owner].contains(name.text));
    }

    void prepareReceivers() {
        if (!receiverSlot) return;
        implicitReceivers.resize(fn.syntax.expressions.size());
        for (ExprId id = 0; id < implicitReceivers.size(); ++id) {
            const auto& node = fn.syntax.expressions[id].node;
            const auto* name = std::get_if<Identifier>(&node);
            const auto* call = std::get_if<Call>(&node);
            if (call && !call->receiver && !call->function && !call->typeOwner) name = &call->name;
            if (name && implicitMember(*name, call != nullptr)) {
                auto site = name->span;
                implicitReceivers[id] = fn.syntax.expressions.size();
                fn.syntax.expressions.push_back({site, This{}});
            }
        }
        fn.expressions.resize(fn.syntax.expressions.size());
    }

    void resolveReceiver(ExprId id) {
        auto& node = fn.syntax.expressions[id].node;
        if (id < implicitReceivers.size() && implicitReceivers[id]) {
            auto receiver = *implicitReceivers[id];
            if (auto* name = std::get_if<Identifier>(&node); name && !lookupLocal(*name)) node = Member{receiver, *name};
            else if (auto* call = std::get_if<Call>(&node); call && !call->typeOwner && !lookupLocal(call->name)) call->receiver = receiver;
        }
        if (auto* call = std::get_if<Call>(&node); call && call->receiver) {
            auto* name = std::get_if<Identifier>(&fn.syntax.expressions[*call->receiver].node);
            if (name && symbols.records.contains(linkedName(*name)) && !lookupLocal(*name) && !implicitMember(*name)) {
                call->typeOwner = TypeQualifier{*name};
                call->receiver.reset();
            }
        }
    }

    void run() {
        scopes.emplace_back();
        for (std::size_t index = fn.syntax.hasReceiver; index < fn.syntax.parameters.size(); ++index) declare(fn.syntax.parameters[index].name, index);
        if (fn.syntax.foreignSymbol) return;
        if (fn.syntax.hasReceiver) receiverSlot = 0;
        if (fn.syntax.initializer) {
            receiverSlot = fn.locals.size();
            fn.locals.push_back({fn.result, false});
        }
        prepareReceivers();
        if (fn.syntax.initializer) {
            visit(fn.syntax.body, {}, true);
            checkInitialization(fn, module.aggregates[fn.result.id], source);
        } else visit(fn.syntax.body, fn.result);
    }

    Type visit(ExprId id, std::optional<Type> expected = {}, bool discard = false) {
        auto type = inspect(id, expected, discard);
        return fit(id, type, expected);
    }

    Type inspect(ExprId id, std::optional<Type> hint, bool discard = false) {
        resolveReceiver(id);
        const auto& expr = fn.syntax.expressions[id];
        return fn.expressions[id].type = std::visit([&](const auto& node) { return infer(id, node, hint, discard); }, expr.node);
    }

    Type fit(ExprId id, Type type, std::optional<Type> expected) {
        auto& info = fn.expressions[id];
        if (expected && type != *expected && type != Type::Never) {
            auto site = fn.syntax.expressions[id].span;
            if (!losslessNumber(type, *expected) && (expected->kind != Type::Option || module.sumTypes[expected->id].success != type)) {
                info.conversion = directConversion(symbols.conversions, module, symbols.sources, type, *expected, source, site);
                if (!info.conversion) throw mismatch(module, source, site, *expected, type);
            }
            info.sourceType = type;
            type = *expected;
        }
        return info.type = type;
    }

    std::optional<Type> numericHint(std::optional<Type> type) const {
        if (type && type->kind == Type::Option) type = module.sumTypes[type->id].success;
        return type && numericType(*type) ? type : std::nullopt;
    }

    void checkIntegerLiteral(Span span, Type type, IntegerLiteral literal) const {
        if (!represents(type, literal)) throw at(source, span, "LYL0002", "整数字面量超出 " + numberName(type) + " 的范围");
    }

    Type infer(ExprId id, IntegerLiteral literal, std::optional<Type> expected, bool) {
        auto type = numericHint(expected).value_or(Type::Int);
        if (type.kind == Type::Real) {
            double magnitude = numberWidth(type) == 32 ? static_cast<double>(static_cast<float>(literal.magnitude)) : static_cast<double>(literal.magnitude);
            fn.expressions[id].realLiteral = literal.negative ? -magnitude : magnitude;
        } else checkIntegerLiteral(fn.syntax.expressions[id].span, type, literal);
        return type;
    }

    Type infer(ExprId id, const RealLiteral& literal, std::optional<Type> expected, bool) {
        auto target = numericHint(expected);
        auto width = literal.width.value_or(target && target->kind == Type::Real ? numberWidth(*target) : 64);
        auto type = number(Type::Real, width);
        const auto* start = literal.digits.data();
        const auto* end = start + literal.digits.size();
        auto parse = [&](auto real) {
            auto [stopped, error] = std::from_chars(start, end, real);
            if (error != std::errc{} || stopped != end) {
                auto message = error == std::errc::result_out_of_range ? "浮点字面量超出 " + numberName(type) + " 的范围" : "无效的浮点字面量";
                throw at(source, fn.syntax.expressions[id].span, "LYL0002", message);
            }
            return static_cast<double>(real);
        };
        fn.expressions[id].realLiteral = width == 32 ? parse(float{}) : parse(double{});
        return type;
    }
    Type infer(ExprId, bool, auto, bool) { return Type::Bool; }
    Type infer(ExprId, const StringLiteral&, auto, bool) { return Type::String; }
    Type infer(ExprId, const BytesLiteral&, auto, bool) { return Type::Bytes; }
    Type infer(ExprId, CharLiteral, auto, bool) { return Type::Char; }

    Type infer(ExprId id, const DisplayValue& display, auto, bool) {
        auto type = visit(display.value);
        if (type == Type::Never) return type;
        if (builtInDisplay(type)) return Type::String;
        if (type.kind == Type::Variable) {
            if (const auto* method = symbols.contracts.member(type, "toString"); method && method->parameters.empty() && !method->asynchronous && method->result == Type::String) return Type::String;
        } else if (type.kind == Type::Record) {
            const auto& record = module.aggregates[type.id];
            const auto& methods = symbols.methods[record.declaration];
            if (auto found = methods.find("toString"); found != methods.end()) {
                const auto& method = symbols.callables[found->second];
                const auto& syntax = symbols.declaration(method);
                if (auto shape = symbols.methodShape(type, method, source); shape && shape->parameters.empty() && !shape->asynchronous && shape->result == Type::String) {
                    if (variables.empty()) fn.expressions[id].target = method.generic ? symbols.instantiate(found->second, record.arguments, source, syntax.name.span) : method.target;
                    return Type::String;
                }
            }
        }
        auto error = at(source, fn.syntax.expressions[display.value].span, "LYT0016", typeName(module, type) + " 未满足 Display，无法用于字符串插值");
        error.hint = "提供可见的同步实例方法 fn toString(): String，或在插值中显式转换。";
        throw error;
    }

    Type infer(ExprId, const TextTemplate& text, auto, bool) {
        bool exits = false;
        for (auto part : text.parts) exits |= visit(part, Type::String) == Type::Never;
        return exits ? Type::Never : Type::String;
    }

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
        auto owner = visit(index.receiver);
        auto offset = visit(index.offset, Type::Int);
        if (owner == Type::Never || offset == Type::Never) return Type::Never;
        if (owner == Type::String) return Type::Char;
        if (owner == Type::Bytes) return number(Type::UInt, 8);
        if (owner.kind != Type::List) throw at(source, index.site, "LYT0014", "下标需要 List、String 或 Bytes，实际是 " + typeName(module, owner));
        return module.listElements[owner.id];
    }

    Type infer(ExprId id, const EmptyOption&, std::optional<Type> expected, bool) {
        if (!expected || expected->kind != Type::Option) throw at(source, fn.syntax.expressions[id].span, "LYT0010", "None/null 需要明确的 Option 目标类型");
        return *expected;
    }

    Type infer(ExprId id, const Propagate& propagation, std::optional<Type> expected, bool) {
        std::optional<Type> hint;
        auto target = returnType();
        if (!target) {
            bool lambda = std::holds_alternative<Lambda>(fn.syntax.expressions[workers.back().id].node);
            throw at(source, propagation.site, "LYT0013", lambda ? "请给 lambda 标注 Option 或 Result 的返回类型以使用 ?" : "请给 blocking 块明确的 Task<Option<T>> 或 Task<Result<T,E>> 目标类型以使用 ?");
        }
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
        return call && !call->receiver && !call->function && !call->typeOwner && linkedName(call->name) == "Some" &&
            !lookupLocal(call->name) && !implicitMember(call->name, true) && call->arguments.size() == 1 && untypedOption(call->arguments.front().value);
    }

    bool needsArgumentHint(ExprId id) const {
        if (untypedOption(id)) return true;
        const auto& node = fn.syntax.expressions[id].node;
        if (auto* literal = std::get_if<ListLiteral>(&node)) return literal->elements.empty();
        return std::holds_alternative<Lambda>(node);
    }

    CheckedPattern sumPattern(const Pattern& pattern, Type subject, SumTag tag) {
        if (subject.kind != (optionTag(tag) ? Type::Option : Type::Result))
            throw at(source, pattern.span, "LYT0011", std::string(tagName(tag)) + " 不能匹配 " + typeName(module, subject));
        if (tag == SumTag::None) return {PatternConstructor(tag)};
        const auto& fields = module.sumTypes[subject.id];
        auto payload = successTag(tag) ? fields.success : fields.failure;
        return {PatternConstructor(tag), {checkPattern(pattern.parts.front(), payload)}};
    }

    CheckedPattern variantPattern(const Pattern& pattern, Type subject, const PatternName& name, std::size_t index) {
        const auto& item = module.enums[subject.id];
        const auto& entry = item.syntax.cases[index];
        const auto& fields = item.payloads[index];
        if (pattern.parts.size() != fields.size()) throw at(source, pattern.span, "LYT0011", name.name.text + " 模式需要 " + std::to_string(fields.size()) + " 个载荷模式");
        CheckedPattern result{PatternConstructor(VariantTag{subject.id, static_cast<unsigned>(index)}), std::vector<CheckedPattern>(fields.size())};
        if (!name.labels) return result;
        std::vector<ParameterShape> parameters;
        for (const auto& field : entry.fields) parameters.push_back({field.name ? std::string_view(field.name->text) : std::string_view{}, false, false});
        std::vector<ArgumentShape> shapes;
        for (std::size_t part = 0; part < pattern.parts.size(); ++part) {
            const auto& label = (*name.labels)[part];
            shapes.push_back({label ? std::optional<std::string_view>(label->text) : std::nullopt, false, label ? label->span : pattern.parts[part].span});
        }
        auto plan = bindArguments(parameters, shapes, source, name.name.span, name.name.text);
        for (std::size_t part = 0; part < pattern.parts.size(); ++part) {
            auto slot = plan.explicitSlots[part].index;
            result.parts[slot] = checkPattern(pattern.parts[part], fields[slot]);
        }
        return result;
    }

    CheckedPattern namedPattern(const Pattern& pattern, Type subject, const PatternName& named) {
        const auto& name = named.name;
        if (named.owner) {
            auto owner = symbols.enums.find(linkedName(*named.owner));
            if (owner == symbols.enums.end()) throw at(source, named.owner->span, "LYT0011", "分支模式需要可见的 enum：" + named.owner->text);
            if (subject != Type{Type::Enum, owner->second}) throw at(source, named.owner->span, "LYT0011", named.owner->text + " 不能匹配 " + typeName(module, subject));
            return variantPattern(pattern, subject, named, variantIndex(subject, name));
        }
        if (!named.escaped || named.labels) {
            if (subject.kind == Type::Enum) {
                const auto& variants = symbols.variants[subject.id];
                if (auto found = variants.find(name.text); found != variants.end()) return variantPattern(pattern, subject, named, found->second);
            }
            if (!named.escaped) if (auto tag = sumTag(name.text)) {
                auto count = *tag == SumTag::None ? 0u : 1u;
                if (pattern.parts.size() != count || (*tag == SumTag::None && named.labels) || (count && named.labels && named.labels->front()))
                    throw at(source, pattern.span, "LYT0011", name.text + (count ? " 模式需要一个位置载荷" : " 模式不接收载荷"));
                return sumPattern(pattern, subject, *tag);
            }
        }
        if (named.labels) {
            auto found = symbols.records.find(linkedName(name));
            if (found == symbols.records.end() || symbols.recordDeclarations[found->second].reference)
                throw at(source, name.span, "LYT0011", "此模式需要可见的 struct 或匹配类型的枚举分支：" + name.text);
            if (subject.kind != Type::Record || module.aggregates[subject.id].declaration != found->second)
                throw at(source, name.span, "LYT0011", name.text + " 不能匹配 " + typeName(module, subject));
            const auto& owner = module.aggregates[subject.id];
            CheckedPattern result{PatternConstructor(PatternProduct{}), std::vector<CheckedPattern>(owner.fields.size())};
            std::vector<bool> used(owner.fields.size());
            for (std::size_t index = 0; index < named.labels->size(); ++index) {
                const auto& field = (*named.labels)[index];
                if (!field) throw at(source, pattern.parts[index].span, "LYT0011", "struct 解构需要字段名");
                auto slot = memberIndex(subject, *field, symbols.fields);
                if (used[slot]) throw at(source, field->span, "LYN0002", "解构模式重复指定字段：" + field->text);
                used[slot] = true;
                visible(owner.syntax.file, owner.syntax.fields[slot].isPublic, *field);
                result.parts[slot] = checkPattern(pattern.parts[index], owner.fields[slot]);
            }
            return result;
        }
        auto slot = fn.locals.size();
        declare(name, slot);
        fn.locals.push_back({subject, false, currentWorker()});
        return {PatternSlot{slot}};
    }

    CheckedPattern checkPattern(const Pattern& pattern, Type subject) {
        const auto& node = pattern.node;
        if (auto* named = std::get_if<PatternName>(&node)) return namedPattern(pattern, subject, *named);
        if (auto* tag = std::get_if<SumTag>(&node)) return sumPattern(pattern, subject, *tag);
        if (std::holds_alternative<std::monostate>(node)) return {};
        if (auto* literal = std::get_if<IntegerLiteral>(&node)) {
            if (!integerType(subject)) throw at(source, pattern.span, "LYT0011", "整数字面量不能匹配 " + typeName(module, subject));
            checkIntegerLiteral(pattern.span, subject, *literal);
            return {integerBits(*literal)};
        }
        if (auto* literal = std::get_if<CharLiteral>(&node)) {
            if (subject != Type::Char) throw at(source, pattern.span, "LYT0011", "字符字面量不能匹配 " + typeName(module, subject));
            return {static_cast<std::uint64_t>(literal->point)};
        }
        auto literal = std::holds_alternative<bool>(node) ? Type::Bool : Type::String;
        if (subject != literal)
            throw at(source, pattern.span, "LYT0011", typeName(module, literal) + " 字面量不能匹配 " + typeName(module, subject));
        if (auto* flag = std::get_if<bool>(&node)) return {PatternConstructor(*flag)};
        return {std::get<StringLiteral>(node)};
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
        bool discardArms = discard && match.optional == OptionalForm::None;
        auto& checked = fn.expressions[id].patterns;
        checked.reserve(match.arms.size());
        std::vector<const CheckedPattern*> patterns;
        std::optional<Type> result;
        for (const auto& arm : match.arms) {
            scopes.emplace_back();
            checked.push_back(checkPattern(arm.pattern, subject));
            if (!arm.guard) patterns.push_back(&checked.back());
            bool mayEnter = !arm.guard || visit(*arm.guard, Type::Bool) != Type::Never;
            auto hint = result ? result : expected;
            auto ignoreValue = discardArms || !mayEnter;
            Type value;
            if (match.optional == OptionalForm::Access && &arm == &match.arms.front()) {
                value = visit(arm.value);
                if (value != Type::Never && value.kind != Type::Option) value = fit(arm.value, value, symbols.sum(Type::Option, value));
            } else if (match.optional == OptionalForm::Access) {
                if (!hint || hint->kind != Type::Option) hint = symbols.sum(Type::Option, Type::Unit);
                value = visit(arm.value, hint);
            } else if (fallback && &arm == &match.arms.back()) value = *fallback;
            else value = visit(arm.value, ignoreValue ? std::nullopt : hint, ignoreValue);
            if (mayEnter && value != Type::Never && !result) result = discardArms ? Type::Unit : value;
            scopes.pop_back();
        }
        if (auto missing = missingPattern(module, subject, patterns)) throw at(source, fn.syntax.expressions[id].span, "LYT0012", "match 未穷尽，缺少分支：" + *missing);
        return discard && match.optional != OptionalForm::None ? Type{Type::Unit} : result.value_or(Type::Never);
    }

    std::size_t captureSlot(std::size_t slot) {
        for (const auto& worker : workers) if (slot < worker.firstLocal) {
            fn.locals[slot].captured = true;
            auto& captures = fn.expressions[worker.id].captures;
            if (std::ranges::find(captures, slot) == captures.end()) captures.push_back(slot);
        }
        return slot;
    }

    std::optional<std::size_t> localSlot(const Identifier& name) {
        if (auto slot = lookupLocal(name)) return captureSlot(*slot);
        return {};
    }

    Type infer(ExprId id, This, auto, bool) {
        if (!receiverSlot) throw at(source, fn.syntax.expressions[id].span, "LYT0018", "this 只能用于实例方法或 init");
        fn.expressions[id].target = captureSlot(*receiverSlot);
        return fn.locals[*receiverSlot].type;
    }

    Type infer(ExprId id, const Identifier& name, std::optional<Type> expected, bool) {
        if (auto slot = localSlot(name)) {
            fn.expressions[id].target = *slot;
            return fn.locals[*slot].type;
        }
        auto found = symbols.functions.find(name.linked.empty() ? name.text : name.linked);
        if (found == symbols.functions.end()) throw at(source, name.span, "LYN0001", "找不到绑定：" + name.text);
        const auto& item = symbols.callables[found->second];
        if (item.generic) {
            if (expected && expected->kind == Type::Option) expected = module.sumTypes[expected->id].success;
            if (!expected || expected->kind != Type::Closure) throw at(source, name.span, "LYG0002", "泛型函数值需要明确的函数类型：" + name.text);
            const auto& shape = module.signatures[expected->id];
            if (shape.parameters.size() != item.generic->parameters.size()) throw at(source, name.span, "LYT0015", "函数值的形参数量与目标类型不符");
            std::vector<GenericArgument<Type>> inputs;
            for (std::size_t slot = 0; slot < shape.parameters.size(); ++slot) {
                auto type = shape.parameters[slot];
                if (item.generic->parameters[slot].variadic) {
                    if (type.kind != Type::List) throw at(source, name.span, "LYT0015", "变长函数作为函数值时，最后一个形参接收 List");
                    type = module.listElements[type.id];
                }
                inputs.push_back({slot, type, name.span});
            }
            auto wanted = std::optional(shape.result);
            if (item.generic->isAsync) wanted = shape.result.kind == Type::Task ? std::optional(module.taskResults[shape.result.id]) : std::nullopt;
            auto types = inferGenericArguments<Type>(*item.generic, Call{name, {}}, name.span, {}, inputs, wanted, source, [&](Type value) { return typeName(module, value); },
                    [&](Type value, const std::string& key, std::size_t slot) { return symbols.project(value, key, slot); }, [](Type, Type) -> std::optional<Type> { return std::nullopt; });
            symbols.requireBounds(*item.generic, types, source, name.span);
            if (variables.empty()) fn.expressions[id].functionRef = symbols.instantiate(found->second, types, source, name.span);
            std::vector<Type> parameters;
            for (const auto& param : item.generic->parameters) {
                auto type = symbols.resolve(*item.generic, param.type, types);
                parameters.push_back(param.variadic ? symbols.list(type) : type);
            }
            auto result = symbols.resolve(*item.generic, item.generic->result, types);
            auto type = symbols.closure(std::move(parameters), item.generic->isAsync ? symbols.task(result) : result);
            fn.expressions[id].callableType = type;
            return type;
        }
        const auto& target = module.functions[item.target];
        std::vector<Type> parameters;
        for (std::size_t index = 0; index < target.syntax.parameters.size(); ++index) parameters.push_back(target.locals[index].type);
        auto type = symbols.closure(std::move(parameters), target.syntax.isAsync ? symbols.task(target.result) : target.result);
        fn.expressions[id].functionRef = item.target;
        fn.expressions[id].callableType = type;
        return type;
    }

    Type infer(ExprId id, const Lambda& lambda, std::optional<Type> expected, bool) {
        if (expected && expected->kind == Type::Option) expected = module.sumTypes[expected->id].success;
        std::vector<std::optional<Type>> hints;
        if (expected && expected->kind == Type::Closure) {
            const auto& shape = module.signatures[expected->id];
            for (auto param : shape.parameters) hints.push_back(param);
            hints.push_back(shape.result);
        } else if (auto found = lambdaHints.find(id); found != lambdaHints.end()) hints = found->second;
        auto errorSite = fn.syntax.expressions[id].span;
        if (lambda.implicit && (hints.empty() || hints.size() > 2)) throw at(source, {errorSite.begin, errorSite.begin + 1}, "LYT0015", "尾部块需要零参或单参函数；多个参数请写显式 lambda");
        auto count = lambda.implicit ? hints.size() - 1 : lambda.parameters.size();
        if (!hints.empty() && count + 1 != hints.size()) throw at(source, errorSite, "LYT0015", "lambda 的形参数量与目标函数类型不符");
        std::optional<Type> result = hints.empty() ? std::nullopt : hints.back();
        if (lambda.result) result = symbols.resolve(fn.syntax, *lambda.result, variables);
        workers.push_back({id, fn.locals.size(), result});
        scopes.emplace_back();
        auto previous = std::exchange(loops, 0U);
        std::vector<Type> parameters;
        for (std::size_t index = 0; index < count; ++index) {
            auto name = lambda.implicit ? Identifier{"it", {errorSite.begin, errorSite.begin + 1}} : lambda.parameters[index].name;
            auto type = hints.empty() ? std::nullopt : hints[index];
            if (!lambda.implicit && lambda.parameters[index].type) type = symbols.resolve(fn.syntax, *lambda.parameters[index].type, variables);
            if (!type) throw at(source, name.span, "LYT0015", "无法推断 lambda 形参类型：" + name.text);
            parameters.push_back(*type);
            auto slot = fn.locals.size();
            declare(name, slot);
            fn.locals.push_back({*type, false, id});
            fn.expressions[id].argumentSlots.push_back(slot);
        }
        auto tail = visit(lambda.body, result);
        result = workers.back().result.value_or(tail);
        fit(lambda.body, tail, result);
        scopes.pop_back();
        workers.pop_back();
        loops = previous;
        return symbols.closure(std::move(parameters), *result);
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

    std::vector<ArgumentShape> argumentShapes(const Call& call, bool acceptsTail) const {
        std::vector<ArgumentShape> shapes;
        for (const auto& arg : call.arguments) {
            auto site = arg.spread.value_or(fn.syntax.expressions[arg.value].span);
            std::optional<std::string_view> label;
            if (arg.label) { site = arg.label->span; label = arg.label->text; }
            if (arg.trailing && !acceptsTail) throw at(source, *arg.trailing, "LYT0015", "尾部 lambda 需要固定的末尾函数形参");
            shapes.push_back({label, arg.spread.has_value(), arg.trailing.value_or(site), arg.trailing.has_value()});
        }
        return shapes;
    }

    template<class GetName, class GetType>
    bool arguments(ExprId id, const Call& call, std::size_t count, GetName getName, GetType getType) {
        std::vector<ParameterShape> parameters;
        for (std::size_t slot = 0; slot < count; ++slot) parameters.push_back({getName(slot), false, false});
        auto last = count ? std::optional<Type>(getType(count - 1)) : std::nullopt;
        auto shapes = argumentShapes(call, last && last->kind == Type::Closure);
        if (count != shapes.size() && (shapes.empty() || !shapes.back().trailing)) throw at(source, call.name.span, "LYT0002", call.name.text + " 需要 " + std::to_string(count) + " 个实参");
        auto plan = bindArguments(parameters, shapes, source, call.name.span, call.name.text);
        bool exits = false;
        for (std::size_t index = 0; index < call.arguments.size(); ++index) {
            auto slot = plan.explicitSlots[index].index;
            fn.expressions[id].argumentSlots.push_back(slot);
            exits |= visit(call.arguments[index].value, getType(slot)) == Type::Never;
        }
        return exits;
    }

    template<class GetType>
    bool functionArguments(ExprId id, const Call& call, const Function& target, GetType getType, bool deferContextual = false) {
        auto skip = static_cast<std::size_t>(call.receiver.has_value());
        std::vector<ParameterShape> parameters;
        for (auto slot = skip; slot < target.parameters.size(); ++slot) {
            const auto& parameter = target.parameters[slot];
            parameters.push_back({parameter.name.text, parameter.defaultValue.has_value(), parameter.variadic});
        }
        auto arguments = argumentShapes(call, !parameters.empty() && !parameters.back().variadic && target.parameters.back().type.text.starts_with("$fn<"));
        auto plan = bindArguments(parameters, arguments, source, call.name.span, call.name.text);
        auto& info = fn.expressions[id];
        for (auto slot : plan.defaults) info.omittedDefaults.push_back(slot + skip);
        bool exits = false;
        std::vector<std::size_t> order, contextual;
        std::vector<bool> checked(call.arguments.size());
        for (std::size_t index = 0; index < call.arguments.size(); ++index) {
            info.argumentSlots.push_back(plan.explicitSlots[index].index);
            auto& pending = deferContextual && needsArgumentHint(call.arguments[index].value) ? contextual : order;
            pending.push_back(index);
        }
        order.insert(order.end(), contextual.begin(), contextual.end());
        for (auto index : order) {
            auto selected = plan.explicitSlots[index];
            std::optional<Type> type = getType(selected.index + skip, index, checked);
            if (type && selected.mode == ArgumentMode::Spread) type = symbols.list(*type);
            auto expr = call.arguments[index].value;
            auto inferred = inspect(expr, type);
            if (selected.mode == ArgumentMode::Item && type && inferred.kind == Type::List && module.listElements[inferred.id] == *type) {
                auto error = mismatch(module, source, arguments[index].site, *type, inferred);
                error.hint = "若要逐项传入列表，请在实参前写 ...。";
                throw error;
            }
            auto value = fit(expr, inferred, type);
            checked[index] = true;
            if (value == Type::Never) exits = true;
            else if (selected.mode == ArgumentMode::Spread && value.kind != Type::List) throw at(source, arguments[index].site, "LYT0001", "展开需要 List，实际是 " + typeName(module, value));
        }
        return exits;
    }

    std::size_t memberIndex(Type owner, const Identifier& name, const std::vector<Names>& tables) const {
        if (owner.kind == Type::Record) {
            const auto& names = tables[module.aggregates[owner.id].declaration];
            if (auto found = names.find(name.text); found != names.end()) return found->second;
        }
        throw at(source, name.span, "LYN0001", typeName(module, owner) + " 没有成员：" + name.text);
    }

    void visible(std::size_t declaringFile, bool isPublic, const Identifier& name) const {
        if (!isPublic && symbols.sources[declaringFile].moduleId != source.moduleId) throw at(source, name.span, "LYN0003", "此成员是模块私有的：" + name.text);
    }

    std::optional<Type> enumNamespace(ExprId receiver) {
        auto* name = std::get_if<Identifier>(&fn.syntax.expressions[receiver].node);
        if (name && !lookupLocal(*name) && !implicitMember(*name))
            if (auto item = symbols.enums.find(linkedName(*name)); item != symbols.enums.end()) return Type{Type::Enum, item->second};
        return std::nullopt;
    }

    std::size_t variantIndex(Type owner, const Identifier& name) const {
        const auto& variants = symbols.variants[owner.id];
        if (auto found = variants.find(name.text); found != variants.end()) return found->second;
        throw at(source, name.span, "LYN0001", typeName(module, owner) + " 没有分支：" + name.text);
    }

    Type infer(ExprId id, const Member& member, auto, bool) {
        if (auto item = enumNamespace(member.receiver)) {
            auto index = variantIndex(*item, member.name);
            if (!module.enums[item->id].payloads[index].empty()) throw at(source, member.name.span, "LYT0002", "带载荷的分支需要传入实参：" + member.name.text);
            auto& info = fn.expressions[id];
            info.callee = Callee::Variant;
            info.target = index;
            return *item;
        }
        auto owner = visit(member.receiver);
        if (owner == Type::Never) return owner;
        if (owner.kind == Type::List && member.name.text == "size") return Type::Int;
        if (owner == Type::String && (member.name.text == "size" || member.name.text == "byteSize")) return Type::Int;
        if (owner == Type::Bytes && member.name.text == "size") return Type::Int;
        if (owner == Type::Char && member.name.text == "codePoint") return number(Type::UInt, 32);
        if (owner == Type::Exception && member.name.text == "message") return Type::String;
        auto field = memberIndex(owner, member.name, symbols.fields);
        fn.expressions[id].target = field;
        const auto& record = module.aggregates[owner.id];
        visible(record.syntax.file, record.syntax.fields[field].isPublic, member.name);
        return record.fields[field];
    }

    std::vector<std::optional<Type>> argumentHints(ExprId id, const Call& call, const Function& target,
            const std::vector<bool>& checked, std::span<const Type> known, std::optional<Type> expected) const {
        std::vector<std::optional<Type>> inferred(target.typeParameters.size());
        std::copy(known.begin(), known.end(), inferred.begin());
        auto learn = [&](auto&& self, const TypeForm& key, Type type, bool widen) -> void {
            if (auto param = genericParameter(target, key.name)) {
                if (!inferred[*param]) inferred[*param] = type;
                else if (widen && *param >= known.size()) if (auto common = commonInteger(*inferred[*param], type)) inferred[*param] = *common;
                return;
            }
            for (std::size_t part = 0; part < key.arguments.size(); ++part) if (auto value = symbols.project(type, key.name, part)) self(self, key.arguments[part], *value, false);
        };
        auto skip = static_cast<std::size_t>(call.receiver.has_value());
        for (std::size_t prior = 0; prior < checked.size(); ++prior) if (checked[prior]) {
            auto type = fn.expressions[call.arguments[prior].value].type;
            if (type == Type::Never) continue;
            if (call.arguments[prior].spread) type = module.listElements[type.id];
            learn(learn, typeForm(target.parameters[fn.expressions[id].argumentSlots[prior] + skip].type.text), type, true);
        }
        if (expected) learn(learn, typeForm(target.result.text), *expected, false);
        return inferred;
    }

    std::pair<std::vector<Type>, bool> bindGeneric(ExprId id, const Call& call, const Function& target, std::optional<Type> expected, std::span<const Type> ownerTypes = {}) {
        auto explicitCount = target.typeParameters.size() - ownerTypes.size();
        if (!call.typeArguments.empty() && call.typeArguments.size() != explicitCount) {
            throw at(source, call.name.span, "LYG0001", call.name.text + " 需要 " + std::to_string(explicitCount) + " 个类型实参");
        }
        std::vector<Type> explicitTypes(ownerTypes.begin(), ownerTypes.end());
        for (const auto& type : call.typeArguments) explicitTypes.push_back(symbols.resolve(fn.syntax, type, variables));
        auto skip = static_cast<std::size_t>(call.receiver.has_value());
        if (target.isAsync && expected) expected = expected->kind == Type::Task ? std::optional(module.taskResults[expected->id]) : std::nullopt;
        bool exits = functionArguments(id, call, target, [&](std::size_t slot, std::size_t index, const std::vector<bool>& checked) -> std::optional<Type> {
                    const auto& annotation = target.parameters[slot].type;
                    auto form = typeForm(annotation.text);
                    auto resolved = symbols.resolveWith(form, symbols.sources[target.file], annotation.span, target.typeParameters, [&](std::size_t parameter) -> std::optional<Type> {
                        return parameter < explicitTypes.size() ? std::optional(explicitTypes[parameter]) : std::nullopt;
                    });
                    auto expr = call.arguments[index].value;
                    if (!resolved && needsArgumentHint(expr)) {
                        auto known = argumentHints(id, call, target, checked, explicitTypes, expected);
                        auto hint = [&](const TypeForm& item) { return symbols.resolveWith(item, symbols.sources[target.file], annotation.span, target.typeParameters, [&](std::size_t param) { return known[param]; }); };
                        if (std::holds_alternative<Lambda>(fn.syntax.expressions[expr].node) && form.name == "$fn") {
                            auto& hints = lambdaHints[expr];
                            for (const auto& part : form.arguments) hints.push_back(hint(part));
                        } else resolved = hint(form);
                    }
                    return resolved;
                }, true);
        auto& info = fn.expressions[id];
        std::vector<GenericArgument<Type>> actuals;
        for (std::size_t index = 0; index < call.arguments.size(); ++index) {
            auto expr = call.arguments[index].value;
            auto type = fn.expressions[expr].type;
            if (call.arguments[index].spread && type != Type::Never) type = module.listElements[type.id];
            actuals.push_back({info.argumentSlots[index] + skip, type, fn.syntax.expressions[expr].span});
        }
        auto types = inferGenericArguments<Type>(target, call, fn.syntax.expressions[id].span, explicitTypes, actuals, expected, source, [&](Type type) { return typeName(module, type); },
                [&](Type value, const std::string& name, std::size_t slot) { return symbols.project(value, name, slot); }, commonInteger);
        symbols.requireBounds(target, types, source, call.name.span);
        for (std::size_t index = 0; index < actuals.size(); ++index) {
            const auto& argument = actuals[index];
            auto type = symbols.resolve(target, target.parameters[argument.slot].type, types);
            auto expr = call.arguments[index].value;
            fit(expr, fn.expressions[expr].type, call.arguments[index].spread ? symbols.list(type) : type);
        }
        return {std::move(types), exits};
    }

    Type genericCall(std::size_t declaration, ExprId id, const Call& call, std::optional<Type> expected, std::span<const Type> ownerTypes) {
        const auto& target = *symbols.callables[declaration].generic;
        auto [types, exits] = bindGeneric(id, call, target, expected, ownerTypes);
        if (variables.empty()) fn.expressions[id].target = symbols.instantiate(declaration, types, source, call.name.span);
        auto result = symbols.resolve(target, target.result, types);
        return exits ? Type{Type::Never} : target.isAsync ? symbols.task(result) : result;
    }

    Type infer(ExprId id, const Call& call, std::optional<Type> expected, bool) {
        auto& info = fn.expressions[id];
        auto noTypeArgs = [&] { if (!call.typeArguments.empty()) throw at(source, call.typeArguments.front().span, "LYG0001", "此调用不接受类型实参：" + call.name.text); };
        auto invoke = [&](Type type) -> Type {
            info.callee = Callee::Indirect;
            if (type == Type::Never) return type;
            if (type.kind != Type::Closure) throw at(source, call.name.span, "LYT0015", "此值不能调用，实际是 " + typeName(module, type));
            noTypeArgs();
            const auto& shape = module.signatures[type.id];
            info.callableType = type;
            for (const auto& argument : call.arguments) if (argument.label) throw at(source, argument.label->span, "LYT0002", "函数类型没有形参名，请按位置传参");
            return arguments(id, call, shape.parameters.size(), [](auto) { return ""; }, [&](auto slot) { return shape.parameters[slot]; }) ? Type{Type::Never} : shape.result;
        };
        if (call.function) return invoke(visit(*call.function));
        const auto& key = call.name.linked.empty() ? call.name.text : call.name.linked;
        if (!call.receiver && !call.typeOwner) if (auto local = localSlot(call.name)) { info.target = *local; return invoke(fn.locals[*local].type); }
        std::size_t declaration;
        std::span<const Type> ownerTypes;
        if (call.typeOwner) {
            const auto& qualifier = *call.typeOwner;
            auto named = symbols.records.find(linkedName(qualifier.name));
            if (named == symbols.records.end()) throw at(source, qualifier.name.span, "LYT0003", "静态方法需要可见的 Struct 或 Class 类型");
            auto owner = named->second;
            auto member = symbols.staticMethods[owner].find(call.name.text);
            if (member == symbols.staticMethods[owner].end()) {
                if (symbols.methods[owner].contains(call.name.text) || symbols.fields[owner].contains(call.name.text))
                    throw at(source, call.name.span, "LYT0018", "实例成员需要对象接收者：" + call.name.text);
                throw at(source, call.name.span, "LYN0001", "找不到静态方法：" + call.name.text);
            }
            declaration = member->second;
            const auto& method = symbols.declaration(symbols.callables[declaration]);
            visible(method.file, method.isPublic, call.name);
            if (!qualifier.arguments.empty()) {
                std::vector<Type> arguments;
                for (const auto& type : qualifier.arguments) arguments.push_back(symbols.resolve(fn.syntax, type, variables));
                auto record = symbols.recordType(owner, std::move(arguments), source, qualifier.name.span);
                ownerTypes = module.aggregates[record.id].arguments;
            } else if (!call.typeArguments.empty() && !symbols.recordDeclarations[owner].typeParameters.empty()) {
                throw at(source, call.name.span, "LYG0001", "显式指定静态方法的类型实参时，先指定所属类型的类型实参，例如 Box<Int>.make<String>(...)");
            }
        } else if (call.receiver) {
            if (auto item = enumNamespace(*call.receiver)) {
                noTypeArgs();
                auto index = variantIndex(*item, call.name);
                const auto& decl = module.enums[item->id];
                info.callee = Callee::Variant;
                info.target = index;
                bool exits = arguments(id, call, decl.payloads[index].size(), [&](auto slot) -> std::string_view {
                    const auto& label = decl.syntax.cases[index].fields[slot].name;
                    return label ? std::string_view(label->text) : std::string_view{};
                }, [&](auto slot) { return decl.payloads[index][slot]; });
                return exits ? Type{Type::Never} : *item;
            }
            auto owner = visit(*call.receiver);
            if (owner == Type::Never) return owner;
            if ((call.name.text == "toString" && (builtInDisplay(owner) || owner == Type::Bytes)) || (call.name.text == "toBytes" && owner == Type::String)) {
                noTypeArgs();
                if (!call.arguments.empty()) throw at(source, call.name.span, "LYT0002", call.name.text + " 不接收实参");
                if (call.name.text == "toBytes") { info.callee = Callee::ToBytes; return Type::Bytes; }
                if (owner == Type::Bytes) { info.callee = Callee::DecodeUtf8; return symbols.sum(Type::Option, Type::String); }
                info.callee = Callee::ToString;
                return Type::String;
            }
            if (numericType(owner) && call.name.text.starts_with("to")) {
                auto name = std::string_view(call.name.text).substr(2);
                bool checked = name.ends_with("Checked");
                if (checked) name.remove_suffix(7);
                if (auto target = numberType(name); target && (checked ? integerType(*target) : target->kind == Type::Real)) {
                    noTypeArgs();
                    if (!call.arguments.empty()) throw at(source, call.name.span, "LYT0002", call.name.text + " 不接收实参");
                    info.callee = checked ? Callee::ToIntegerChecked : Callee::ToReal;
                    return checked ? symbols.sum(Type::Option, *target) : *target;
                }
            }
            if (owner.kind == Type::Variable) {
                if (auto method = symbols.contracts.member(owner, call.name.text)) {
                    noTypeArgs();
                    bool exits = arguments(id, call, method->parameters.size(), [&](auto slot) -> std::string_view { return method->labels[slot]; }, [&](auto slot) { return method->parameters[slot]; });
                    return exits ? Type{Type::Never} : method->asynchronous ? symbols.task(method->result) : method->result;
                }
            }
            if (owner.kind == Type::Record) {
                const auto& decl = module.aggregates[owner.id];
                const auto& fields = symbols.fields[decl.declaration];
                if (auto field = fields.find(call.name.text); field != fields.end()) {
                    visible(decl.syntax.file, decl.syntax.fields[field->second].isPublic, call.name);
                    info.target = field->second;
                    return invoke(decl.fields[info.target]);
                }
                if (symbols.staticMethods[decl.declaration].contains(call.name.text))
                    throw at(source, call.name.span, "LYT0018", "静态方法必须通过类型名调用：" + call.name.text);
                ownerTypes = decl.arguments;
            }
            if (owner.kind == Type::Task && call.name.text == "cancel") {
                if (!workers.empty() || !fn.syntax.isAsync) throw at(source, call.name.span, "LYT0008", "Task.cancel() 只能在 async 函数中调用，不能在同步 lambda、blocking 或 defer 中调用");
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
        } else if (auto tag = sumTag(key); tag && *tag != SumTag::None) {
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
        } else if (key == "Exception") {
            noTypeArgs();
            info.callee = Callee::Exception;
            return arguments(id, call, 1, [](auto) { return "message"; }, [](auto) { return Type{Type::String}; }) ? Type::Never : Type::Exception;
        } else if (key == "print") {
            noTypeArgs();
            info.callee = Callee::Print;
            bool exits = arguments(id, call, 1, [](auto) { return "value"; }, [](auto) { return std::nullopt; });
            auto arg = call.arguments.front().value;
            auto type = fn.expressions[arg].type;
            if (!exits && !builtInDisplay(type)) throw at(source, fn.syntax.expressions[arg].span, "LYT0001", "print 需要数值、Bool、Char 或 String，实际是 " + typeName(module, type));
            return exits ? Type::Never : Type::Unit;
        } else if (key == "gcCollect") {
            noTypeArgs();
            info.callee = Callee::Collect;
            if (!call.arguments.empty()) throw at(source, call.name.span, "LYT0002", "gcCollect 不接收实参");
            return Type::Unit;
        } else if (auto record = symbols.records.find(key); record != symbols.records.end()) {
            const auto& decl = symbols.recordDeclarations[record->second];
            if (expected && expected->kind == Type::Option) expected = module.sumTypes[expected->id].success;
            if (auto initializer = symbols.initializers[record->second]) {
                declaration = *initializer;
                const auto& ctor = symbols.declaration(symbols.callables[declaration]);
                visible(ctor.file, ctor.isPublic, call.name);
            } else {
                info.callee = Callee::Constructor;
                for (const auto& field : decl.fields) visible(decl.file, field.isPublic, call.name);
                if (!decl.typeParameters.empty()) {
                    auto init = constructorSignature(decl);
                    auto [types, exits] = bindGeneric(id, call, init, expected);
                    auto type = symbols.recordType(record->second, std::move(types), source, call.name.span);
                    info.target = type.id;
                    return exits ? Type{Type::Never} : type;
                }
                noTypeArgs();
                auto type = symbols.recordType(record->second, {}, source, call.name.span);
                info.target = type.id;
                const auto& instance = module.aggregates[type.id];
                bool exits = arguments(id, call, instance.fields.size(), [&](auto slot) -> const auto& { return decl.fields[slot].name.text; }, [&](auto slot) { return instance.fields[slot]; });
                return exits ? Type{Type::Never} : type;
            }
        } else {
            auto found = symbols.functions.find(key);
            if (found == symbols.functions.end()) throw at(source, call.name.span, "LYN0001", "找不到函数：" + call.name.text);
            declaration = found->second;
        }
        if (symbols.callables[declaration].generic) return genericCall(declaration, id, call, expected, ownerTypes);
        noTypeArgs();
        info.target = symbols.callables[declaration].target;
        const auto& target = module.functions[info.target];
        bool exits = functionArguments(id, call, target.syntax, [&](auto slot, auto, const auto&) {
            auto type = target.locals[slot].type;
            return target.syntax.parameters[slot].variadic ? module.listElements[type.id] : type;
        });
        return exits ? Type{Type::Never} : target.syntax.isAsync ? symbols.task(target.result) : target.result;
    }

    Type infer(ExprId, const TaskOp& op, std::optional<Type> expected, bool) {
        if (!workers.empty() || !fn.syntax.isAsync) throw at(source, op.site, "LYT0008", "await 和 spawn 只能在 async 函数中使用，不能用于同步 lambda、blocking 或 defer");
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
        if (auto index = std::get_if<Index>(&expr.node)) return fn.expressions[index->receiver].type.kind == Type::List;
        if (std::holds_alternative<Identifier>(expr.node)) return !fn.expressions[id].callableType && fn.locals[fn.expressions[id].target].writable;
        if (auto member = std::get_if<Member>(&expr.node)) {
            auto type = fn.expressions[member->receiver].type;
            if (type.kind != Type::Record) return false;
            const auto& owner = module.aggregates[type.id].syntax;
            return owner.fields[fn.expressions[id].target].writable && (owner.reference || writable(member->receiver));
        }
        return false;
    }

    Type infer(ExprId, const Unary& unary, std::optional<Type> expected, bool) {
        if (unary.op == Op::Not) return visit(unary.operand, Type::Bool);
        auto type = inspect(unary.operand, numericHint(expected));
        if (type == Type::Never || type.kind == Type::Int || type.kind == Type::Real) return type;
        if (type.kind == Type::UInt) throw at(source, unary.site, "LYT0006", "无符号整数不能取负");
        auto hint = numericHint(expected);
        return fit(unary.operand, type, hint && hint->kind != Type::UInt ? *hint : Type{Type::Int});
    }

    Type infer(ExprId, const Binary& binary, std::optional<Type> expected, bool) {
        if (binary.op <= Op::RemSet) {
            const auto& place = fn.syntax.expressions[binary.left];
            auto type = visit(binary.left);
            if (type == Type::Never) { visit(binary.right); return type; }
            auto* member = std::get_if<Member>(&place.node);
            bool initialize = fn.syntax.initializer && workers.empty() && binary.op == Op::Set && member &&
                std::holds_alternative<This>(fn.syntax.expressions[member->receiver].node);
            if (!initialize && !writable(binary.left)) throw at(source, place.span, "LYT0004", "不能给此位置赋值；需要 var 绑定或可写字段");
            if (binary.op != Op::Set && !numericType(type) && !(binary.op == Op::AddSet && type == Type::String)) throw mismatch(module, source, place.span, Type::Int, type);
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
        auto hint = binary.op > Op::Ge ? numericHint(expected) : std::nullopt;
        auto integerLiteral = [&](ExprId id) { return std::holds_alternative<IntegerLiteral>(fn.syntax.expressions[id].node); };
        auto realLiteral = [&](ExprId id) { return std::get_if<RealLiteral>(&fn.syntax.expressions[id].node); };
        auto contextual = [&](ExprId id) { auto* real = realLiteral(id); return integerLiteral(id) || (real && !real->width); };
        Type left, right;
        if (contextual(binary.left) && (!contextual(binary.right) || (integerLiteral(binary.left) && realLiteral(binary.right)))) {
            right = inspect(binary.right, hint.value_or(integerLiteral(binary.left) ? Type::Int : Type::Real));
            left = inspect(binary.left, numericType(right) ? std::optional(right) : hint);
        } else {
            left = inspect(binary.left, hint);
            auto rightHint = numericType(left) && !contextual(binary.right) ? std::nullopt : left == Type::Never ? hint : std::optional(left);
            right = inspect(binary.right, rightHint);
        }
        if (left == Type::Never || right == Type::Never) return Type::Never;
        if (numericType(left) || (numericType(right) && !equality)) {
            if (!numericType(left)) left = fit(binary.left, left, right);
            if (!numericType(right)) right = fit(binary.right, right, left);
            auto common = commonInteger(left, right);
            if (left.kind == Type::Real && right.kind == Type::Real) common = number(Type::Real, std::max(numberWidth(left), numberWidth(right)));
            if (!common) {
                auto error = at(source, binary.site, "LYT0001", numberName(left) + " 与 " + numberName(right) + " 没有共同的无损数值类型");
                error.hint = "请显式转换其中一个操作数。";
                throw error;
            }
            fit(binary.left, left, *common);
            fit(binary.right, right, *common);
            return binary.op <= Op::Ge ? Type{Type::Bool} : *common;
        }
        right = fit(binary.right, right, left);
        if (equality && (left.kind == Type::Option || left.kind == Type::Result || left.kind == Type::Enum || left.kind == Type::Closure || left.kind == Type::Variable || (left.kind == Type::Record && !module.aggregates[left.id].syntax.reference))) throw at(source, binary.site, "LYT0006", typeName(module, left) + " 未提供 Eq，不能用 == 或 != 比较");
        if (binary.op == Op::Add && left == Type::String) return Type::String;
        if (!equality) throw mismatch(module, source, binary.site, Type::Int, left);
        return Type::Bool;
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
        if (fn.syntax.initializer && workers.empty()) {
            if (ret.value) throw at(source, fn.syntax.expressions[id].span, "LYT0017", "init 只能用不带值的 return；构造结果始终是 this");
            return Type::Never;
        }
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

    struct IterationMethod { Type result; std::optional<std::size_t> function; };

    IterationMethod iterationMethod(Type owner, const std::string& name, Span site) {
        if (owner.kind == Type::Variable) {
            if (const auto* method = symbols.contracts.member(owner, name); method && method->parameters.empty() && !method->asynchronous)
                return {method->result, {}};
        } else if (owner.kind == Type::Record) {
            const auto& record = module.aggregates[owner.id];
            const auto& methods = symbols.methods[record.declaration];
            if (auto found = methods.find(name); found != methods.end()) {
                const auto& entry = symbols.callables[found->second];
                const auto& target = symbols.declaration(entry);
                visible(target.file, target.isPublic, {name, site});
                if (auto shape = symbols.methodShape(owner, entry, source); shape && shape->parameters.empty() && !shape->asynchronous) {
                    std::optional<std::size_t> function;
                    if (variables.empty()) function = entry.generic ? symbols.instantiate(found->second, record.arguments, source, site) : entry.target;
                    return {shape->result, function};
                }
            }
        }
        auto expected = name == "iterator" ? "可见的同步 iterator()，或使用 List、整数区间" : "可见的同步 next(): Option<T>";
        throw at(source, site, "LYT0014", "for ... in 需要" + std::string(expected) + "，实际是 " + typeName(module, owner));
    }

    Type infer(ExprId id, const For& loop, auto, bool) {
        auto input = visit(loop.input, loop.range ? std::optional(Type{Type::Int}) : std::nullopt);
        auto element = input;
        bool exits = input == Type::Never;
        if (loop.range) {
            exits |= visit(loop.range->end, Type::Int) == Type::Never;
            element = Type::Int;
        } else if (input.kind == Type::List) {
            element = module.listElements[input.id];
        } else if (input == Type::String) {
            element = Type::Char;
        } else if (input == Type::Bytes) {
            element = number(Type::UInt, 8);
        } else if (!exits) {
            auto site = fn.syntax.expressions[loop.input].span;
            auto iterator = iterationMethod(input, "iterator", site);
            auto next = iterationMethod(iterator.result, "next", site);
            if (next.result.kind != Type::Option)
                throw at(source, site, "LYT0014", "for ... in 的 next() 必须返回 Option<T>，实际是 " + typeName(module, next.result));
            element = module.sumTypes[next.result.id].success;
            if (iterator.function && next.function) fn.expressions[id].iterator = IteratorPlan{*iterator.function, *next.function};
        }
        scopes.emplace_back();
        auto slot = fn.locals.size();
        declare(loop.item, slot);
        fn.locals.push_back({element, false, currentWorker()});
        fn.expressions[id].target = slot;
        ++loops;
        visit(loop.body, {}, true);
        --loops;
        scopes.pop_back();
        return exits ? Type::Never : Type::Unit;
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
    auto reserveType = [&](const auto& decl, Names& names, std::size_t index) {
        const auto& declaration = decl.name.text;
        auto separator = declaration.rfind("::");
        auto name = std::string_view(declaration).substr(separator == declaration.npos ? 0 : separator + 2);
        if (numberType(name)) throw at(sources[decl.file], decl.name.span, "LYN0002", "类型名与内置类型冲突：" + std::string(name));
        for (Type builtin : scalarTypes) {
            if (name == typeName(checked, builtin)) throw at(sources[decl.file], decl.name.span, "LYN0002", "类型名与内置类型冲突：" + std::string(name));
        }
        if (name == "Never") throw at(sources[decl.file], decl.name.span, "LYN0002", "类型名与内置类型冲突：Never");
        if (decl.name.text == "print" || decl.name.text == "gcCollect" || !names.emplace(declaration, index).second) throw at(sources[decl.file], decl.name.span, "LYN0002", "类型名称已声明：" + decl.name.text);
    };
    for (auto& decl : module.aggregates) {
        reserveType(decl, symbols.records, symbols.recordDeclarations.size());
        symbols.recordDeclarations.push_back(std::move(decl));
    }
    for (auto& decl : module.enums) {
        if (symbols.records.contains(decl.name.text)) throw at(sources[decl.file], decl.name.span, "LYN0002", "类型名称已声明：" + decl.name.text);
        reserveType(decl, symbols.enums, checked.enums.size());
        checked.enums.push_back({std::move(decl), {}});
    }
    symbols.fields.resize(symbols.recordDeclarations.size());
    symbols.methods.resize(symbols.recordDeclarations.size());
    symbols.staticMethods.resize(symbols.recordDeclarations.size());
    symbols.initializers.resize(symbols.recordDeclarations.size());
    symbols.variants.resize(checked.enums.size());
    for (std::size_t id = 0; id < symbols.recordDeclarations.size(); ++id) {
        const auto& syntax = symbols.recordDeclarations[id];
        const auto& source = sources[syntax.file];
        std::set<std::string> seen;
        std::vector<Type> variables;
        for (const auto& parameter : syntax.typeParameters) {
            if (!seen.insert(parameter.text).second) throw at(source, parameter.span, "LYN0002", "类型形参重复：" + parameter.text);
            variables.emplace_back(Type::Variable, checked.variableNames.size());
            checked.variableNames.push_back(parameter.text);
        }
        for (std::size_t slot = 0; slot < syntax.fields.size(); ++slot) {
            const auto& field = syntax.fields[slot];
            if (!symbols.fields[id].emplace(field.name.text, slot).second) throw at(source, field.name.span, "LYN0002", "字段重复：" + field.name.text);
        }
        symbols.recordType(id, std::move(variables), source, syntax.name.span);
    }
    for (const auto& trait : module.traits) {
        const auto& source = sources[trait.file];
        if (symbols.records.contains(trait.name.text) || symbols.enums.contains(trait.name.text)) throw at(source, trait.name.span, "LYN0002", "类型名称已声明：" + trait.name.text);
        symbols.contracts.define(trait, source, [&](const Identifier& type) { return symbols.resolve(source, type); });
    }
    for (std::size_t id = 0; id < checked.enums.size(); ++id) {
        auto& item = checked.enums[id];
        const auto& source = sources[item.syntax.file];
        for (const auto& entry : item.syntax.cases) {
            if (!symbols.variants[id].emplace(entry.name.text, item.payloads.size()).second) throw at(source, entry.name.span, "LYN0002", "枚举分支重复：" + entry.name.text);
            auto& payload = item.payloads.emplace_back();
            std::set<std::string> labels;
            for (const auto& field : entry.fields) {
                if (field.name && !labels.insert(field.name->text).second) throw at(source, field.name->span, "LYN0002", "枚举分支载荷名重复：" + field.name->text);
                payload.push_back(symbols.resolve(source, field.type));
            }
        }
    }
    for (auto& function : module.functions) {
        const auto& source = sources[function.file];
        auto owner = function.owner ? std::optional(symbols.records.at(function.owner->text)) : std::nullopt;
        if (function.initializer) symbols.initializers[*owner] = symbols.callables.size();
        else {
            auto& names = !owner ? symbols.functions : function.hasReceiver ? symbols.methods[*owner] : symbols.staticMethods[*owner];
            bool collision = owner ? symbols.fields[*owner].contains(function.name.text) ||
                (function.hasReceiver ? symbols.staticMethods[*owner] : symbols.methods[*owner]).contains(function.name.text)
                : symbols.records.contains(function.name.text) || symbols.enums.contains(function.name.text) || function.name.text == "print" || function.name.text == "gcCollect";
            if (collision || !names.emplace(function.name.text, symbols.callables.size()).second)
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
            for (const auto& bound : function.bounds) symbols.contracts.bind(variables[bound.parameter], bound.trait, source);
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
    validateForeignDeclarations(checked, sources);
    if (requireEntry && !checked.entry) throw Diagnostic{"LYN0001", "可执行包缺少入口 fn main(): Unit"};
    return checked;
}

}
