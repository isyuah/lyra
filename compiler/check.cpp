module;
#include <algorithm>
#include <charconv>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

std::optional<Type> hiddenType(const CheckedModule& module, Type type) {
    auto arguments = [&](std::span<const Type> types) -> std::optional<Type> {
        for (auto nested : types) if (auto hidden = hiddenType(module, nested)) return hidden;
        return {};
    };
    switch (type.kind) {
        case Type::Record: {
            const auto& record = module.aggregates[type.id];
            return record.syntax.isPublic ? arguments(record.arguments) : std::optional(type);
        }
        case Type::Enum: {
            const auto& item = module.enums[type.id];
            return item.syntax.isPublic ? arguments(item.arguments) : std::optional(type);
        }
        case Type::List: return hiddenType(module, module.listElements[type.id]);
        case Type::Task: return hiddenType(module, module.taskResults[type.id]);
        case Type::Option: return hiddenType(module, module.sumTypes[type.id].success);
        case Type::Result: {
            const auto& sum = module.sumTypes[type.id];
            if (auto hidden = hiddenType(module, sum.success)) return hidden;
            return hiddenType(module, sum.failure);
        }
        case Type::Dyn: return module.traits[type.id].isPublic ? std::nullopt : std::optional(type);
        case Type::Closure: {
            const auto& shape = module.signatures[type.id];
            if (auto hidden = arguments(shape.parameters)) return hidden;
            return hiddenType(module, shape.result);
        }
        default: return {};
    }
}

namespace {

using Names = std::unordered_map<std::string, std::size_t>;
using Overloads = std::unordered_map<std::string, std::vector<std::size_t>>;

const std::string& linkedName(const Identifier& name) { return name.linked.empty() ? name.text : name.linked; }

constexpr Type scalarTypes[] = {Type::Bool, Type::String, Type::Char, Type::Bytes, Type::Unit, Type::CPtr, Type::Exception, Type::Any};

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
        case Type::Any: return "Any";
        case Type::Dyn: {
            const auto& full = module.traits[type.id].name.text;
            if (canonical) return "$dyn:" + full;
            auto separator = full.rfind("::");
            return "dyn " + (separator == full.npos ? full : full.substr(separator + 2));
        }
        case Type::Record: case Type::Enum: {
            const auto& full = type.kind == Type::Record ? module.aggregates[type.id].syntax.name.text : module.enums[type.id].syntax.name.text;
            const auto& arguments = type.kind == Type::Record ? module.aggregates[type.id].arguments : module.enums[type.id].arguments;
            auto separator = full.rfind("::");
            auto text = canonical || separator == full.npos ? full : full.substr(separator + 2);
            if (arguments.empty()) return text;
            text += '<';
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                if (index) text += canonical ? "," : ", ";
                text += name(arguments[index]);
            }
            return text + '>';
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

struct Callable {
    std::size_t target;
    std::optional<Function> generic;
    std::vector<Type> variables;
    mutable std::optional<Identifier> inferredResult{};
    mutable bool inferring{};
};

struct OverloadKey {
    std::size_t typeParameters;
    std::vector<std::pair<std::string, bool>> parameters;
    bool operator==(const OverloadKey&) const = default;
};

OverloadKey overloadKey(const Function& syntax) {
    OverloadKey key{syntax.typeParameters.size(), {}};
    for (const auto& parameter : syntax.parameters) {
        auto normalized = mapTypeNames(typeForm(parameter.type.text), [&](const std::string& name) {
            if (auto slot = genericParameter(syntax, name)) return "$" + std::to_string(*slot);
            if (auto type = numberType(name)) return numberName(*type);
            return name;
        });
        key.parameters.emplace_back(std::move(normalized), parameter.variadic);
    }
    return key;
}

enum class MatchKind { Exact, Number, Nullable, Literal, User, Box };
struct ArgumentMatch {
    MatchKind kind;
    Type target{};

    std::partial_ordering compare(const ArgumentMatch& other) const {
        auto rank = [](MatchKind kind) { return kind == MatchKind::Exact ? 0 : kind == MatchKind::User ? 2 : kind == MatchKind::Box ? 3 : 1; };
        auto tier = rank(kind) <=> rank(other.kind);
        if (tier != 0) return tier;
        if (kind != other.kind) return std::partial_ordering::unordered;
        if (kind == MatchKind::Literal && target != other.target) return std::partial_ordering::unordered;
        if (kind == MatchKind::Number && integerType(target) && target.kind == other.target.kind)
            return numberWidth(target) <=> numberWidth(other.target);
        return std::partial_ordering::equivalent;
    }
};

struct OverloadMatch {
    std::size_t declaration;
    std::vector<ArgumentMatch> arguments;
    bool generic, variadic;
    std::size_t defaults;

    bool betterThan(const OverloadMatch& other) const {
        bool better = false;
        bool exact = true;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            auto order = arguments[index].compare(other.arguments[index]);
            if (order == std::partial_ordering::unordered || order > 0) return false;
            better |= order < 0;
            exact &= arguments[index].kind == MatchKind::Exact;
        }
        if (better) return true;
        if (exact && generic != other.generic) return !generic;
        if (variadic != other.variadic) return !variadic;
        return defaults < other.defaults;
    }
};

Function defaultFunction(const Function& original, std::size_t slot) {
    auto syntax = original;
    const auto& parameter = original.parameters[slot];
    const auto& owner = original.owner ? original.owner : original.extensionGroup;
    syntax.name.text = (owner ? owner->text + "." : "") + original.name.text + "$default$" + parameter.name.text;
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

Identifier nominalAnnotation(const auto& declaration) {
    auto result = declaration.name;
    if (!declaration.typeParameters.empty()) result.text += '<';
    for (std::size_t index = 0; index < declaration.typeParameters.size(); ++index) {
        if (index) result.text += ',';
        result.text += declaration.typeParameters[index].text;
    }
    if (!declaration.typeParameters.empty()) result.text += '>';
    return result;
}

Function constructorSignature(const Aggregate& record) {
    std::vector<Parameter> parameters;
    for (const auto& field : record.fields) parameters.push_back({field.name, field.type});
    return {record.file, record.name, std::move(parameters), nominalAnnotation(record), {}, 0, {}, record.isPublic, record.typeParameters, false, {}, false, record.bounds};
}

struct Symbols {
    CheckedModule& module;
    const std::vector<Source>& sources;
    Names records;
    Overloads functions;
    std::vector<Names> fields;
    std::vector<Overloads> methods;
    std::vector<Callable> callables;
    std::map<GenericInstanceKey, std::size_t> specializations;
    std::vector<DirectConversion> conversions{};
    StructuralContracts contracts{};
    Names traitNames{};
    Names enums{};
    std::vector<Names> variants{};
    std::vector<Enumeration> enumDeclarations{};
    std::vector<Aggregate> recordDeclarations{};
    std::vector<std::optional<std::size_t>> initializers{};
    std::vector<Overloads> staticMethods{};
    Names extensions{};
    std::vector<Extension> extensionDeclarations{};
    std::vector<Overloads> extensionMethods{};
    std::vector<std::vector<std::size_t>> visibleExtensions{};
    std::map<std::pair<std::size_t, std::vector<Type>>, Type> recordInstances{};
    std::map<std::pair<std::size_t, std::vector<Type>>, Type> enumInstances{};
    unsigned expandingNominal{};
    struct NominalCheck { Type type; const Source* source; Span site; };
    std::vector<NominalCheck> nominalChecks{};
    std::size_t nextNominalCheck{};
    bool boundsReady{};

    struct TypeTrial {
        Symbols& symbols;
        std::size_t aggregates = symbols.module.aggregates.size(), enums = symbols.module.enums.size(), tasks = symbols.module.taskResults.size(),
            sums = symbols.module.sumTypes.size(), lists = symbols.module.listElements.size(), signatures = symbols.module.signatures.size();
        unsigned expanding = symbols.expandingNominal;
        std::size_t nominal = symbols.nominalChecks.size(), nextNominal = symbols.nextNominalCheck;

        ~TypeTrial() {
            if (symbols.module.aggregates.size() != aggregates) {
                std::erase_if(symbols.recordInstances, [&](const auto& entry) { return entry.second.id >= aggregates; });
                symbols.module.aggregates.resize(aggregates);
            }
            if (symbols.module.enums.size() != enums) {
                std::erase_if(symbols.enumInstances, [&](const auto& entry) { return entry.second.id >= enums; });
                symbols.module.enums.resize(enums);
            }
            symbols.expandingNominal = expanding;
            symbols.nominalChecks.resize(nominal);
            symbols.nextNominalCheck = nextNominal;
            symbols.module.taskResults.resize(tasks);
            symbols.module.sumTypes.resize(sums);
            symbols.module.listElements.resize(lists);
            symbols.module.signatures.resize(signatures);
        }
    };

    const Function& declaration(const Callable& callable) const { return callable.generic ? *callable.generic : module.functions[callable.target].syntax; }

    const Identifier& resultAnnotation(const Callable& candidate, const Source& use, Span site);

    std::string declarationSite(const Function& syntax) const {
        auto site = sources[syntax.file].userSite(syntax.name.span);
        return site.file + ':' + std::to_string(site.start.line) + ':' + std::to_string(site.start.column);
    }

    std::vector<std::size_t> visibleCandidates(std::span<const std::size_t> indexes, const Source& use, bool exportedOnly = false) const {
        std::vector<std::size_t> result;
        for (auto index : indexes) {
            const auto& syntax = declaration(callables[index]);
            if (syntax.isPublic || (!exportedOnly && sources[syntax.file].moduleId == use.moduleId)) result.push_back(index);
        }
        return result;
    }

    std::vector<std::size_t> visibleFunctions(const Source& use, const Identifier& name) const {
        auto found = functions.find(linkedName(name));
        return found == functions.end() ? std::vector<std::size_t>{} : visibleCandidates(found->second, use, name.exportedOnly);
    }

    std::size_t enclosingParameters(const Function& syntax) const {
        if (syntax.extensionGroup) return extensionDeclarations[extensions.at(syntax.extensionGroup->text)].typeParameters.size();
        return syntax.owner ? recordDeclarations[records.at(syntax.owner->text)].typeParameters.size() : 0;
    }

    std::string signature(std::size_t index, const std::string& spelling) const {
        const auto& syntax = declaration(callables[index]);
        auto format = [](const Identifier& identifier) {
            return mapTypeNames(typeForm(identifier.text), [](const std::string& name) {
                auto separator = name.rfind("::");
                return separator == name.npos ? name : name.substr(separator + 2);
            });
        };
        std::string text = spelling;
        auto owning = enclosingParameters(syntax);
        if (syntax.typeParameters.size() > owning) {
            text += '<';
            for (auto slot = owning; slot < syntax.typeParameters.size(); ++slot) {
                if (slot != owning) text += ", ";
                text += syntax.typeParameters[slot].text;
            }
            text += '>';
        }
        text += '(';
        for (auto slot = static_cast<std::size_t>(syntax.hasReceiver); slot < syntax.parameters.size(); ++slot) {
            const auto& parameter = syntax.parameters[slot];
            if (slot != syntax.hasReceiver) text += ", ";
            text += format(parameter.type);
            if (parameter.variadic) text += "...";
        }
        text += ')';
        if (syntax.extensionGroup) {
            const auto& group = syntax.extensionGroup->text;
            auto separator = group.rfind("::");
            text += " in " + (separator == group.npos ? group : group.substr(separator + 2));
        }
        return text + " @ " + declarationSite(syntax);
    }

    std::optional<RequiredMethod> methodShape(Type receiver, const Callable& candidate, const Source& use) {
        const auto& record = module.aggregates[receiver.id];
        const auto& actual = declaration(candidate);
        if ((!actual.isPublic && sources[actual.file].moduleId != use.moduleId) || actual.typeParameters.size() != record.arguments.size()) return {};
        auto type = [&](const Identifier& annotation) { return resolve(actual, annotation, record.arguments); };
        RequiredMethod result{{}, {}, type(resultAnnotation(candidate, use, actual.name.span)), actual.isAsync};
        for (std::size_t slot = 1; slot < actual.parameters.size(); ++slot) {
            const auto& parameter = actual.parameters[slot];
            if (parameter.variadic) return {};
            result.labels.push_back(parameter.name.text);
            result.parameters.push_back(type(parameter.type));
        }
        return result;
    }

    std::optional<std::pair<std::size_t, RequiredMethod>> shapedMethod(Type receiver, const std::string& name, const Source& use, const auto& accept) {
        if (receiver.kind != Type::Record) return {};
        const auto& members = methods[module.aggregates[receiver.id].declaration];
        auto found = members.find(name);
        if (found == members.end()) return {};
        for (auto index : found->second) {
            auto shape = methodShape(receiver, callables[index], use);
            if (shape && accept(*shape)) return std::pair(index, std::move(*shape));
        }
        return {};
    }

    void requireBounds(std::span<const Bound> bounds, std::size_t file, std::span<const Type> types, const Source& use, Span site) {
        contracts.require(bounds, types, use, site, [&](Type receiver, const std::string& name, const RequiredMethod& expected) -> std::optional<RequiredMethod> {
            if (receiver.kind == Type::Dyn) {
                if (auto method = contracts.member(module.traits[receiver.id].name.text, name)) return *method;
                return {};
            }
            const auto& definition = sources[file];
            auto result = shapedMethod(receiver, name, definition, [&](const auto& shape) { return shape == expected; });
            if (!result) result = shapedMethod(receiver, name, definition, [](const auto&) { return true; });
            return result ? std::optional(std::move(result->second)) : std::nullopt;
        }, [&](Type type) { return typeName(module, type); });
    }

    void requireBounds(const Function& target, std::span<const Type> types, const Source& use, Span site) {
        requireBounds(target.bounds, target.file, types, use, site);
    }

    void validateNominalBounds() {
        if (!boundsReady || expandingNominal) return;
        while (nextNominalCheck < nominalChecks.size()) {
            const auto pending = nominalChecks[nextNominalCheck++];
            auto validate = [&](const auto& instance) {
                requireBounds(instance.syntax.bounds, instance.syntax.file, instance.arguments, *pending.source, pending.site);
            };
            if (pending.type.kind == Type::Record) validate(module.aggregates[pending.type.id]);
            else validate(module.enums[pending.type.id]);
        }
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
        auto argument = [&](const auto& instance) -> std::optional<Type> {
            if (instance.syntax.name.text == name && slot < instance.arguments.size()) return instance.arguments[slot];
            return {};
        };
        if (value.kind == Type::Record) return argument(module.aggregates[value.id]);
        if (value.kind == Type::Enum) return argument(module.enums[value.id]);
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

    std::optional<std::vector<Type>> extensionTypes(std::size_t index, Type receiver) {
        const auto& group = extensionDeclarations[index];
        std::vector<std::optional<Type>> inferred(group.typeParameters.size());
        auto match = [&](auto&& self, const TypeForm& form, Type actual) -> bool {
            if (form.arguments.empty()) {
                auto parameter = std::ranges::find(group.typeParameters, form.name, &Identifier::text);
                if (parameter == group.typeParameters.end()) return actual == resolveForm(form, sources[group.file], group.receiver.span, nullptr, {});
                auto& result = inferred[parameter - group.typeParameters.begin()];
                if (!result) result = actual;
                return *result == actual;
            }
            if (form.name == "$fn" && (actual.kind != Type::Closure || form.arguments.size() != module.signatures[actual.id].parameters.size() + 1)) return false;
            for (std::size_t slot = 0; slot < form.arguments.size(); ++slot) {
                auto argument = project(actual, form.name, slot);
                if (!argument || !self(self, form.arguments[slot], *argument)) return false;
            }
            return true;
        };
        if (!match(match, typeForm(group.receiver.text), receiver)) return {};
        std::vector<Type> result;
        for (auto type : inferred) result.push_back(type.value());
        return result;
    }

    bool abstract(Type type) const {
        switch (type.kind) {
            case Type::Variable: return true;
            case Type::Record: return module.aggregates[type.id].abstract;
            case Type::Enum: return module.enums[type.id].abstract;
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

    Type recordType(std::size_t declaration, std::vector<Type> arguments, const Source& source, Span span) {
        const auto& syntax = recordDeclarations[declaration];
        if (arguments.size() != syntax.typeParameters.size())
            throw at(source, span, "LYG0001", syntax.name.text + " 需要 " + std::to_string(syntax.typeParameters.size()) + " 个类型实参");
        if (auto found = recordInstances.find({declaration, arguments}); found != recordInstances.end()) return found->second;
        if (expandingNominal >= 128 || recordInstances.size() >= 4096)
            throw at(source, span, "LYG0004", "记录类型实例展开过深或超出编译单元实例预算：" + syntax.name.text);
        Type type{Type::Record, module.aggregates.size()};
        bool symbolic = std::ranges::any_of(arguments, [&](Type argument) { return abstract(argument); });
        recordInstances.emplace(std::pair{declaration, arguments}, type);
        auto& record = module.aggregates.emplace_back(syntax, std::vector<Type>{}, declaration, std::move(arguments), symbolic);
        ++expandingNominal;
        for (const auto& field : syntax.fields) {
            auto resolved = resolveWith(typeForm(field.type.text), sources[syntax.file], field.type.span, syntax.typeParameters,
                [&](std::size_t slot) { return record.arguments[slot]; });
            record.fields.push_back(resolved.value());
        }
        --expandingNominal;
        if (!syntax.bounds.empty()) nominalChecks.push_back({type, &source, span});
        validateNominalBounds();
        return type;
    }

    Type enumType(std::size_t declaration, std::vector<Type> arguments, const Source& source, Span span) {
        const auto& syntax = enumDeclarations[declaration];
        if (arguments.size() != syntax.typeParameters.size())
            throw at(source, span, "LYG0001", syntax.name.text + " 需要 " + std::to_string(syntax.typeParameters.size()) + " 个类型实参");
        if (auto found = enumInstances.find({declaration, arguments}); found != enumInstances.end()) return found->second;
        if (expandingNominal >= 128 || enumInstances.size() >= 4096)
            throw at(source, span, "LYG0004", "枚举类型实例展开过深或超出编译单元实例预算：" + syntax.name.text);
        Type type{Type::Enum, module.enums.size()};
        bool symbolic = std::ranges::any_of(arguments, [&](Type argument) { return abstract(argument); });
        enumInstances.emplace(std::pair{declaration, arguments}, type);
        auto& instance = module.enums.emplace_back(syntax, std::vector<std::vector<Type>>{}, declaration, std::move(arguments), symbolic);
        ++expandingNominal;
        for (const auto& entry : syntax.cases) {
            auto& payload = instance.payloads.emplace_back();
            for (const auto& field : entry.fields) {
                auto resolved = resolveWith(typeForm(field.type.text), sources[syntax.file], field.type.span, syntax.typeParameters,
                    [&](std::size_t slot) { return instance.arguments[slot]; });
                payload.push_back(resolved.value());
            }
        }
        --expandingNominal;
        if (!syntax.bounds.empty()) nominalChecks.push_back({type, &source, span});
        validateNominalBounds();
        return type;
    }

    template<class Lookup>
    std::optional<Type> resolveWith(const TypeForm& form, const Source& source, Span span, std::span<const Identifier> parameters, const Lookup& lookup) {
        if (form.arguments.empty()) {
            auto found = std::ranges::find(parameters, form.name, &Identifier::text);
            if (found != parameters.end()) return lookup(static_cast<std::size_t>(found - parameters.begin()));
            if (auto type = numberType(form.name)) return type;
            if (form.name.starts_with("$dyn:")) {
                auto trait = traitNames.find(form.name.substr(5));
                if (trait == traitNames.end()) throw at(source, span, "LYG0005", "找不到动态接口 trait：" + form.name.substr(5));
                return Type{Type::Dyn, trait->second};
            }
            for (Type type : scalarTypes) if (form.name == typeName(module, type)) return type;
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
        if (auto item = enums.find(form.name); item != enums.end()) return enumType(item->second, std::move(arguments), source, span);
        throw at(source, span, "LYT0003", "找不到类型或类型实参数量不符：" + form.name);
    }

    Type resolveForm(const TypeForm& form, const Source& source, Span span, const Function* signature, std::span<const Type> types) {
        return resolveWith(form, source, span, signature ? std::span<const Identifier>(signature->typeParameters) : std::span<const Identifier>{}, [&](std::size_t slot) { return types[slot]; }).value();
    }

    Type resolve(const Source& source, const Identifier& name) {
        return resolveForm(typeForm(name.text), source, name.span, nullptr, {});
    }

    Type resolve(const Function& signature, const Identifier& name, std::span<const Type> types) {
        return resolveForm(typeForm(name.text), sources[signature.file], name.span, &signature, types);
    }

    CheckedFunction prepare(Function syntax, std::span<const Type> arguments = {}) {
        auto result = syntax.result.text.empty() ? Type{Type::Unit} : resolve(syntax, syntax.result, arguments);
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
        const auto& callable = callables[declaration];
        auto syntax = *callable.generic;
        syntax.result = resultAnnotation(callable, source, site);
        addConcrete(prepare(specializeGeneric(std::move(syntax), key.arguments)));
        specializations.emplace(std::move(key), index);
        return index;
    }

    void bindFieldDefaults(Type type, std::span<const std::size_t> slots, const Source& source, Span site) {
        auto& record = module.aggregates[type.id];
        record.defaultFunctions.resize(record.fields.size());
        for (auto slot : slots) {
            if (record.defaultFunctions[slot]) continue;
            auto helper = functions.at(fieldDefaultName(record.syntax.name.text, record.syntax.fields[slot].name.text)).front();
            record.defaultFunctions[slot] = record.arguments.empty() ? callables[helper].target : instantiate(helper, record.arguments, source, site);
        }
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
    bool probing{};
    std::optional<Type>* inferredReturn{};
    using Present = std::set<std::size_t>;
    Present present{};

    bool materializing() const { return variables.empty() && !probing && !inferredReturn; }

    std::optional<ExprId> currentWorker() const { return workers.empty() ? std::nullopt : std::optional(workers.back().id); }
    std::optional<Type> returnType() const {
        if (!workers.empty()) return workers.back().result;
        return inferredReturn ? *inferredReturn : std::optional(fn.result);
    }

    std::string localKey(const Identifier& name) const {
        auto mark = source.hygieneAt(name.span);
        return mark ? "$macro" + std::to_string(mark) + ":" + name.text : name.text;
    }

    void declare(const Identifier& name, std::size_t slot) {
        if (!scopes.back().emplace(localKey(name), slot).second) throw at(source, name.span, "LYN0002", "名称已在此作用域声明：" + name.text);
        if (!name.text.starts_with('$')) fn.locals[slot].declaration = name.span;
    }

    void refer(const Identifier& name, std::size_t file, Span span) {
        fn.references.push_back({{fn.syntax.file, name.span}, {file, span}});
    }

    void referLocal(const Identifier& name, std::size_t slot) {
        if (auto declaration = fn.locals[slot].declaration) refer(name, fn.syntax.file, *declaration);
    }

    Type readLocal(ExprId id, const Identifier& name, std::size_t slot) {
        referLocal(name, slot);
        auto& info = fn.expressions[id];
        info.target = slot;
        info.narrowed = present.contains(slot);
        auto type = fn.locals[slot].type;
        return info.narrowed ? module.sumTypes[type.id].success : type;
    }

    std::optional<std::size_t> lookupLocal(const Identifier& name) const {
        auto key = localKey(name);
        for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
            if (auto found = scope->find(key); found != scope->end()) return found->second;
        return {};
    }

    std::optional<std::size_t> stableOption(ExprId id) const {
        const auto* name = std::get_if<Identifier>(&fn.syntax.expressions[id].node);
        if (auto slot = name ? lookupLocal(*name) : std::nullopt; slot && !fn.locals[*slot].writable && fn.locals[*slot].type.kind == Type::Option)
            return slot;
        return {};
    }

    Type inspectOption(ExprId id, std::optional<Type> hint = {}) {
        auto slot = stableOption(id);
        if (!slot || !present.erase(*slot)) return inspect(id, hint);
        auto type = inspect(id, hint);
        present.insert(*slot);
        return type;
    }

    static Present bothPaths(const Present& left, const Present& right) {
        Present result;
        for (auto slot : left) if (right.contains(slot)) result.insert(slot);
        return result;
    }

    Present definitelyPresent(ExprId id, bool outcome) const {
        const auto& condition = fn.syntax.expressions[id].node;
        if (auto* unary = std::get_if<Unary>(&condition); unary && unary->op == Op::Not) return definitelyPresent(unary->operand, !outcome);
        const auto* binary = std::get_if<Binary>(&condition);
        if (!binary) return {};
        if (binary->op == Op::And || binary->op == Op::Or) {
            auto required = binary->op == Op::And;
            auto left = definitelyPresent(binary->left, required);
            auto right = definitelyPresent(binary->right, outcome);
            left.merge(right);
            return outcome == required ? std::move(left) : bothPaths(left, definitelyPresent(binary->left, outcome));
        }
        if ((binary->op == Op::Ne && outcome) || (binary->op == Op::Eq && !outcome)) {
            auto empty = [&](ExprId expr) { return std::holds_alternative<EmptyOption>(fn.syntax.expressions[expr].node); };
            auto slot = empty(binary->left) ? stableOption(binary->right) : empty(binary->right) ? stableOption(binary->left) : std::nullopt;
            if (slot) return {*slot};
        }
        return {};
    }

    bool implicitMember(const Identifier& name, bool call = false) const {
        if (!receiverSlot || source.hygieneAt(name.span)) return false;
        auto receiver = fn.locals[*receiverSlot].type;
        if (receiver.kind != Type::Record) return false;
        auto owner = module.aggregates[receiver.id].declaration;
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

    void enterFunction() {
        scopes.emplace_back();
        for (std::size_t index = fn.syntax.hasReceiver; index < fn.syntax.parameters.size(); ++index) declare(fn.syntax.parameters[index].name, index);
        if (fn.syntax.foreignSymbol) return;
        if (fn.syntax.hasReceiver) receiverSlot = 0;
        if (fn.syntax.initializer) {
            receiverSlot = fn.locals.size();
            fn.locals.push_back({fn.result, false});
        }
        prepareReceivers();
    }

    void run() {
        enterFunction();
        if (fn.syntax.foreignSymbol) return;
        if (fn.syntax.initializer) {
            visit(fn.syntax.body, {}, true);
            checkInitialization(fn, module.aggregates[fn.result.id], source);
        } else visit(fn.syntax.body, fn.result);
    }

    Type inferResult() {
        std::optional<Type> result;
        inferredReturn = &result;
        enterFunction();
        auto tail = visit(fn.syntax.body);
        return result.value_or(tail == Type::Never ? Type{Type::Unit} : tail);
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
            if (*expected != Type::Any && !losslessNumber(type, *expected) && (expected->kind != Type::Option || module.sumTypes[expected->id].success != type)) {
                info.conversion = directConversion(symbols.conversions, module, symbols.sources, type, *expected, source, site, !probing);
                if (!info.conversion) throw mismatch(module, source, site, *expected, type);
            }
            info.sourceType = type;
            type = *expected;
        }
        return info.type = type;
    }

    Type fitArgument(ExprId id, Type actual, std::optional<Type> expected, bool spread) {
        if (spread && expected && actual != *expected && actual.kind == Type::List && module.listElements[expected->id] == Type::Any) {
            fn.expressions[id].boxedSpread = true;
            return actual;
        }
        return fit(id, actual, expected);
    }

    Type infer(ExprId id, const TypeOperation& operation, auto, bool) {
        auto target = symbols.resolve(fn.syntax, operation.type, variables);
        auto actual = visit(operation.value);
        if (actual == Type::Never) return actual;
        fn.expressions[id].testedType = target;
        if (target.kind == Type::Dyn) {
            const auto& trait = module.traits[target.id];
            refer(operation.type, trait.file, trait.name.span);
        }
        if (operation.op == TypeOperator::InterfaceCast) {
            if (target.kind != Type::Dyn) throw at(source, operation.type.span, "LYT0021", "as dyn 需要一个非可空的接口类型");
            if (actual == target) return target;
            const auto& trait = module.traits[target.id];
            Bound bound{0, trait.name};
            symbols.requireBounds({&bound, 1}, fn.syntax.file, {&actual, 1}, source, operation.site);
            if (materializing()) for (const auto& method : trait.methods) {
                auto& implementations = fn.expressions[id].interfaceMethods;
                if (actual.kind == Type::Dyn) {
                    const auto& available = module.traits[actual.id].methods;
                    auto found = std::ranges::find(available, method.name.text, [](const TraitMethod& item) { return item.name.text; });
                    implementations.push_back(found - available.begin());
                } else {
                    const auto& expected = *symbols.contracts.member(trait.name.text, method.name.text);
                    auto selected = symbols.shapedMethod(actual, method.name.text, source, [&](const auto& shape) { return shape == expected; }).value().first;
                    const auto& entry = symbols.callables[selected];
                    auto function = entry.generic ? symbols.instantiate(selected, module.aggregates[actual.id].arguments, source, operation.site) : entry.target;
                    implementations.push_back(function);
                }
            }
            return target;
        }
        if (actual != Type::Any) throw at(source, operation.site, "LYT0021", "is 与 as? 需要 Any 值，实际是 " + typeName(module, actual));
        return operation.op == TypeOperator::Is ? Type{Type::Bool} : symbols.sum(Type::Option, target);
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
            if (auto found = symbols.shapedMethod(type, "toString", source, [](const auto& shape) {
                return shape.parameters.empty() && !shape.asynchronous && shape.result == Type::String;
            })) {
                const auto& method = symbols.callables[found->first];
                const auto& syntax = symbols.declaration(method);
                if (materializing()) fn.expressions[id].target = method.generic ? symbols.instantiate(found->first, record.arguments, source, syntax.name.span) : method.target;
                return Type::String;
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
        auto entry = present;
        auto normal = visit(attempt.body, discard ? std::nullopt : expected, discard);
        auto normalFacts = std::exchange(present, std::move(entry));
        scopes.emplace_back();
        if (const auto& binding = attempt.binding) {
            if (symbols.resolve(fn.syntax, binding->type, variables) != Type::Exception) throw at(source, binding->type.span, "LYE0002", "当前仅支持捕获普通异常根类型 Exception");
            auto slot = fn.locals.size();
            fn.locals.push_back({Type::Exception, false, currentWorker()});
            declare(binding->name, slot);
            fn.expressions[id].target = slot;
        }
        catchOwners.push_back(currentWorker());
        auto recovered = visit(attempt.handler, discard ? std::nullopt : normal != Type::Never ? std::optional(normal) : expected, discard);
        catchOwners.pop_back();
        scopes.pop_back();
        if (normal == Type::Never && recovered == Type::Never) return Type::Never;
        if (recovered == Type::Never) present = std::move(normalFacts);
        else if (normal != Type::Never) present = bothPaths(present, normalFacts);
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
            if (workers.empty()) throw at(source, propagation.site, "LYT0013", "请给函数标注 Option 或 Result 的返回类型以使用 ?");
            bool lambda = std::holds_alternative<Lambda>(fn.syntax.expressions[workers.back().id].node);
            throw at(source, propagation.site, "LYT0013", lambda ? "请给 lambda 标注 Option 或 Result 的返回类型以使用 ?" : "请给 blocking 块明确的 Task<Option<T>> 或 Task<Result<T,E>> 目标类型以使用 ?");
        }
        if (expected && (target->kind == Type::Option || target->kind == Type::Result)) hint = symbols.sum(target->kind, *expected, module.sumTypes[target->id].failure);
        auto operand = inspectOption(propagation.operand, hint);
        if (operand == Type::Never) return operand;
        if (operand.kind != Type::Option && operand.kind != Type::Result) throw at(source, propagation.site, "LYT0013", "? 需要 Option 或 Result，实际是 " + typeName(module, operand));
        if (operand.kind == Type::Result && target->kind == Type::Result && module.sumTypes[target->id].failure != module.sumTypes[operand.id].failure) {
            fn.expressions[id].errorConversion = directConversion(symbols.conversions, module, symbols.sources, module.sumTypes[operand.id].failure, module.sumTypes[target->id].failure, source, propagation.site, !probing);
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
        auto genericEnum = [&](ExprId receiver) {
            const auto* name = enumNamespace(receiver);
            return name && !symbols.enumDeclarations[symbols.enums.at(linkedName(*name))].typeParameters.empty();
        };
        if (const auto* member = std::get_if<Member>(&node)) return genericEnum(member->receiver);
        if (const auto* call = std::get_if<Call>(&node); call && call->receiver && call->arguments.empty()) return genericEnum(*call->receiver);
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
        const auto& syntax = module.enums[subject.id].syntax;
        refer(name.name, syntax.file, syntax.cases[index].name.span);
        if (name.owner) refer(*name.owner, syntax.file, syntax.name.span);
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
            if (subject.kind != Type::Enum || module.enums[subject.id].declaration != owner->second)
                throw at(source, named.owner->span, "LYT0011", named.owner->text + " 不能匹配 " + typeName(module, subject));
            return variantPattern(pattern, subject, named, variantIndex(owner->second, name));
        }
        if (!named.escaped || named.labels) {
            if (subject.kind == Type::Enum) {
                const auto& variants = symbols.variants[module.enums[subject.id].declaration];
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
            refer(name, owner.syntax.file, owner.syntax.name.span);
            CheckedPattern result{PatternConstructor(PatternProduct{}), std::vector<CheckedPattern>(owner.fields.size())};
            std::vector<bool> used(owner.fields.size());
            for (std::size_t index = 0; index < named.labels->size(); ++index) {
                const auto& field = (*named.labels)[index];
                if (!field) throw at(source, pattern.parts[index].span, "LYT0011", "struct 解构需要字段名");
                auto slot = memberIndex(subject, *field, symbols.fields);
                if (used[slot]) throw at(source, field->span, "LYN0002", "解构模式重复指定字段：" + field->text);
                used[slot] = true;
                visible(owner.syntax.file, owner.syntax.fields[slot].isPublic, *field);
                refer(*field, owner.syntax.file, owner.syntax.fields[slot].name.span);
                result.parts[slot] = checkPattern(pattern.parts[index], owner.fields[slot]);
            }
            return result;
        }
        auto slot = fn.locals.size();
        fn.locals.push_back({subject, false, currentWorker()});
        declare(name, slot);
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
                auto entry = present;
                fallback = visit(match.arms.back().value);
                present = std::move(entry);
                element = *fallback == Type::Never ? Type{Type::Unit} : *fallback;
            }
            subjectHint = symbols.sum(Type::Option, *element);
        }
        auto subject = match.optional == OptionalForm::None ? inspect(match.subject, subjectHint) : inspectOption(match.subject, subjectHint);
        if (subject == Type::Never) return subject;
        if (match.optional != OptionalForm::None && subject.kind != Type::Option)
            throw at(source, match.operation, "LYT0010", "可空运算符需要 Option，实际是 " + typeName(module, subject));
        bool discardArms = discard && match.optional == OptionalForm::None;
        auto& checked = fn.expressions[id].patterns;
        checked.reserve(match.arms.size());
        std::vector<const CheckedPattern*> patterns;
        std::optional<Type> result;
        auto entry = present;
        for (const auto& arm : match.arms) {
            present = entry;
            scopes.emplace_back();
            checked.push_back(checkPattern(arm.pattern, subject));
            if (!arm.guard) patterns.push_back(&checked.back());
            bool mayEnter = !arm.guard || visit(*arm.guard, Type::Bool) != Type::Never;
            if (arm.guard && mayEnter) present.merge(definitelyPresent(*arm.guard, true));
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
        present = std::move(entry);
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

    std::pair<Type, std::vector<Type>> functionValue(std::size_t index, const Identifier& name, std::optional<Type> expected) {
        const auto& item = symbols.callables[index];
        if (item.generic) {
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
            std::vector<Type> parameters;
            for (const auto& param : item.generic->parameters) {
                auto type = symbols.resolve(*item.generic, param.type, types);
                parameters.push_back(param.variadic ? symbols.list(type) : type);
            }
            auto result = symbols.resolve(*item.generic, symbols.resultAnnotation(item, source, name.span), types);
            return {symbols.closure(std::move(parameters), item.generic->isAsync ? symbols.task(result) : result), std::move(types)};
        }
        const auto& target = module.functions[item.target];
        std::vector<Type> parameters;
        for (std::size_t slot = 0; slot < target.syntax.parameters.size(); ++slot) parameters.push_back(target.locals[slot].type);
        auto result = target.syntax.result.text.empty() ? symbols.resolve(target.syntax, symbols.resultAnnotation(item, source, name.span), {}) : target.result;
        return {symbols.closure(std::move(parameters), target.syntax.isAsync ? symbols.task(result) : result), {}};
    }

    std::size_t selectFunctionValue(const Identifier& name, std::span<const std::size_t> candidates, std::optional<Type> expected) {
        std::vector<std::size_t> matches;
        if (expected && expected->kind == Type::Closure) {
            for (auto candidate : candidates) {
                Symbols::TypeTrial transaction{symbols};
                try {
                    if (functionValue(candidate, name, expected).first == *expected) matches.push_back(candidate);
                } catch (const Diagnostic&) {
                    continue;
                }
            }
            if (std::ranges::any_of(matches, [&](auto index) { return !symbols.callables[index].generic; }))
                std::erase_if(matches, [&](auto index) { return symbols.callables[index].generic.has_value(); });
        }
        if (matches.size() == 1) return matches.front();
        auto error = at(source, name.span, "LYT0019", !expected || expected->kind != Type::Closure ? "重载函数值需要明确的函数类型：" + name.text :
            matches.empty() ? "没有重载签名匹配 " + typeName(module, *expected) : "多个重载签名匹配 " + typeName(module, *expected));
        for (auto index : matches.empty() ? candidates : std::span<const std::size_t>(matches)) error.message += "\n  " + symbols.signature(index, name.text);
        throw error;
    }

    Type infer(ExprId id, const Identifier& name, std::optional<Type> expected, bool) {
        if (auto slot = localSlot(name)) return readLocal(id, name, *slot);
        auto candidates = symbols.visibleFunctions(source, name);
        if (candidates.empty()) throw at(source, name.span, "LYN0001", "找不到绑定：" + name.text);
        if (expected && expected->kind == Type::Option) expected = module.sumTypes[expected->id].success;
        auto index = candidates.size() == 1 ? candidates.front() : selectFunctionValue(name, candidates, expected);
        const auto& item = symbols.callables[index];
        auto [type, types] = functionValue(index, name, expected);
        const auto& syntax = symbols.declaration(item);
        refer(name, syntax.file, syntax.name.span);
        if (!item.generic) fn.expressions[id].functionRef = item.target;
        else if (materializing()) fn.expressions[id].functionRef = symbols.instantiate(index, types, source, name.span);
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
        auto entry = present;
        auto previous = std::exchange(loops, 0U);
        std::vector<Type> parameters;
        for (std::size_t index = 0; index < count; ++index) {
            auto name = lambda.implicit ? Identifier{"it", {errorSite.begin, errorSite.begin + 1}} : lambda.parameters[index].name;
            auto type = hints.empty() ? std::nullopt : hints[index];
            if (!lambda.implicit && lambda.parameters[index].type) type = symbols.resolve(fn.syntax, *lambda.parameters[index].type, variables);
            if (!type) throw at(source, name.span, "LYT0015", "无法推断 lambda 形参类型：" + name.text);
            parameters.push_back(*type);
            auto slot = fn.locals.size();
            fn.locals.push_back({*type, false, id});
            declare(name, slot);
            fn.expressions[id].argumentSlots.push_back(slot);
        }
        auto tail = visit(lambda.body, result);
        result = workers.back().result.value_or(tail);
        fit(lambda.body, tail, result);
        scopes.pop_back();
        workers.pop_back();
        present = std::move(entry);
        loops = previous;
        return symbols.closure(std::move(parameters), *result);
    }

    Type infer(ExprId id, const Binding& binding, auto, bool) {
        std::optional<Type> annotation;
        if (binding.annotation) annotation = symbols.resolve(fn.syntax, *binding.annotation, variables);
        auto value = visit(binding.value, annotation);
        auto slot = fn.locals.size();
        fn.locals.push_back({annotation.value_or(value), binding.writable, currentWorker()});
        declare(binding.name, slot);
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
    bool functionArguments(ExprId id, const Call& call, const Function& target, GetType getType, bool deferContextual = false, std::span<const Field> fields = {}) {
        auto skip = static_cast<std::size_t>(call.receiver.has_value());
        std::vector<ParameterShape> parameters;
        for (auto slot = skip; slot < target.parameters.size(); ++slot) {
            const auto& parameter = target.parameters[slot];
            auto defaulted = fields.empty() ? parameter.defaultValue.has_value() : fields[slot].defaultValue.has_value();
            parameters.push_back({parameter.name.text, defaulted, parameter.variadic});
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
            if (selected.mode == ArgumentMode::Item && type && *type != Type::Any && inferred.kind == Type::List && module.listElements[inferred.id] == *type) {
                auto error = mismatch(module, source, arguments[index].site, *type, inferred);
                error.hint = "若要逐项传入列表，请在实参前写 ...。";
                throw error;
            }
            auto spread = selected.mode == ArgumentMode::Spread;
            if (spread && inferred != Type::Never && inferred.kind != Type::List) {
                auto site = Span{call.arguments[index].spread->begin, fn.syntax.expressions[expr].span.end};
                throw at(source, site, "LYT0001", "展开需要 List，实际是 " + typeName(module, inferred));
            }
            auto value = fitArgument(expr, inferred, type, spread);
            checked[index] = true;
            if (value == Type::Never) exits = true;
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

    const Identifier* enumNamespace(ExprId receiver) const {
        auto* name = std::get_if<Identifier>(&fn.syntax.expressions[receiver].node);
        return name && !lookupLocal(*name) && !implicitMember(*name) && symbols.enums.contains(linkedName(*name)) ? name : nullptr;
    }

    std::size_t variantIndex(std::size_t owner, const Identifier& name) const {
        const auto& variants = symbols.variants[owner];
        if (auto found = variants.find(name.text); found != variants.end()) return found->second;
        throw at(source, name.span, "LYN0001", symbols.enumDeclarations[owner].name.text + " 没有分支：" + name.text);
    }

    Type variant(ExprId id, const TypeQualifier& qualifier, const Call& call, std::optional<Type> expected, bool bare = false) {
        auto declaration = symbols.enums.at(linkedName(qualifier.name));
        const auto& syntax = symbols.enumDeclarations[declaration];
        auto index = variantIndex(declaration, call.name);
        const auto& entry = syntax.cases[index];
        if (bare && !entry.fields.empty()) throw at(source, call.name.span, "LYT0002", "带载荷的分支需要传入实参：" + call.name.text);
        if (!call.typeArguments.empty()) throw at(source, call.typeArguments.front().span, "LYG0001", "在枚举名称后指定类型实参");
        refer(qualifier.name, syntax.file, syntax.name.span);
        refer(call.name, syntax.file, entry.name.span);
        std::vector<Parameter> payload;
        for (const auto& field : entry.fields) payload.push_back({field.name.value_or(Identifier{"", field.type.span}), field.type});
        Function signature{syntax.file, entry.name, std::move(payload), nominalAnnotation(syntax), {}, 0, {}, syntax.isPublic, syntax.typeParameters, false, {}, false, syntax.bounds};
        Call construction{call.name, call.arguments, {}, qualifier.arguments};
        if (expected && expected->kind == Type::Option) expected = module.sumTypes[expected->id].success;
        auto [types, exits] = bindGeneric(id, construction, signature, expected);
        auto& info = fn.expressions[id];
        info.callee = Callee::Variant;
        info.target = index;
        return exits ? Type{Type::Never} : symbols.enumType(declaration, std::move(types), source, qualifier.name.span);
    }

    Type infer(ExprId id, const QualifiedMember& member, std::optional<Type> expected, bool) {
        if (!symbols.enums.contains(linkedName(member.owner.name)))
            throw at(source, member.name.span, "LYP0001", "静态方法调用需要参数列表或尾部 lambda");
        return variant(id, member.owner, Call{member.name, {}}, expected, true);
    }

    Type infer(ExprId id, const Member& member, std::optional<Type> expected, bool) {
        if (const auto* item = enumNamespace(member.receiver)) return variant(id, TypeQualifier{*item}, Call{member.name, {}}, expected, true);
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
        refer(member.name, record.syntax.file, record.syntax.fields[field].name.span);
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
            if (call.arguments[prior].spread) type = fn.expressions[call.arguments[prior].value].boxedSpread ? Type{Type::Any} : module.listElements[type.id];
            learn(learn, typeForm(target.parameters[fn.expressions[id].argumentSlots[prior] + skip].type.text), type, true);
        }
        if (expected && !target.result.text.empty()) learn(learn, typeForm(target.result.text), *expected, false);
        return inferred;
    }

    std::pair<std::vector<Type>, bool> bindGeneric(ExprId id, const Call& call, const Function& target, std::optional<Type> expected, std::span<const Type> ownerTypes = {}, std::span<const Field> fields = {}) {
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
                }, true, fields);
        auto& info = fn.expressions[id];
        std::vector<GenericArgument<Type>> actuals;
        for (std::size_t index = 0; index < call.arguments.size(); ++index) {
            auto expr = call.arguments[index].value;
            auto type = fn.expressions[expr].type;
            if (call.arguments[index].spread && type != Type::Never) type = fn.expressions[expr].boxedSpread ? Type{Type::Any} : module.listElements[type.id];
            actuals.push_back({info.argumentSlots[index] + skip, type, fn.syntax.expressions[expr].span});
        }
        auto types = inferGenericArguments<Type>(target, call, fn.syntax.expressions[id].span, explicitTypes, actuals, expected, source, [&](Type type) { return typeName(module, type); },
                [&](Type value, const std::string& name, std::size_t slot) { return symbols.project(value, name, slot); }, commonInteger);
        symbols.requireBounds(target, types, source, call.name.span);
        for (std::size_t index = 0; index < actuals.size(); ++index) {
            const auto& argument = actuals[index];
            auto type = symbols.resolve(target, target.parameters[argument.slot].type, types);
            auto expr = call.arguments[index].value;
            auto spread = call.arguments[index].spread.has_value();
            fitArgument(expr, fn.expressions[expr].type, spread ? symbols.list(type) : type, spread);
        }
        return {std::move(types), exits};
    }

    Type genericCall(std::size_t declaration, ExprId id, const Call& call, std::optional<Type> expected, std::span<const Type> ownerTypes, bool onlyArguments) {
        const auto& candidate = symbols.callables[declaration];
        const auto& target = *candidate.generic;
        auto [types, exits] = bindGeneric(id, call, target, expected, ownerTypes);
        if (onlyArguments) return Type::Never;
        if (materializing()) fn.expressions[id].target = symbols.instantiate(declaration, types, source, call.name.span);
        auto result = symbols.resolve(target, symbols.resultAnnotation(candidate, source, call.name.span), types);
        return exits ? Type{Type::Never} : target.isAsync ? symbols.task(result) : result;
    }

    Type declaredCall(std::size_t declaration, ExprId id, const Call& call, std::optional<Type> expected, std::span<const Type> ownerTypes = {}, bool onlyArguments = false) {
        const auto& candidate = symbols.callables[declaration];
        const auto& syntax = symbols.declaration(candidate);
        if (!onlyArguments) {
            if (!syntax.initializer) refer(call.name, syntax.file, syntax.name.span);
            fn.calls.push_back({syntax.file, syntax.name.span});
        }
        if (candidate.generic) return genericCall(declaration, id, call, expected, ownerTypes, onlyArguments);
        if (!call.typeArguments.empty()) throw at(source, call.typeArguments.front().span, "LYG0001", "此调用不接受类型实参：" + call.name.text);
        fn.expressions[id].target = candidate.target;
        const auto& target = module.functions[candidate.target];
        bool exits = functionArguments(id, call, target.syntax, [&](auto slot, auto, const auto&) {
            auto type = target.locals[slot].type;
            return target.syntax.parameters[slot].variadic ? module.listElements[type.id] : type;
        });
        if (onlyArguments) return Type::Never;
        auto result = syntax.result.text.empty() ? symbols.resolve(syntax, symbols.resultAnnotation(candidate, source, call.name.span), {}) : target.result;
        return exits ? Type{Type::Never} : target.syntax.isAsync ? symbols.task(result) : result;
    }

    ArgumentMatch argumentMatch(ExprId id) const {
        const auto& info = fn.expressions[id];
        if (info.conversion) return {MatchKind::User};
        if (info.boxedSpread || (info.sourceType && info.type == Type::Any)) return {MatchKind::Box};
        if (info.sourceType) return info.type.kind == Type::Option ? ArgumentMatch{MatchKind::Nullable} : ArgumentMatch{MatchKind::Number, info.type};
        const auto& node = fn.syntax.expressions[id].node;
        if (std::holds_alternative<IntegerLiteral>(node) && info.type != Type::Int) return {MatchKind::Literal, info.type};
        if (const auto* real = std::get_if<RealLiteral>(&node); real && !real->width && info.type != Type::Real) return {MatchKind::Literal, info.type};
        return {MatchKind::Exact};
    }

    std::size_t selectOverloadWith(ExprId id, const Call& call, std::optional<Type> expected, std::span<const std::size_t> candidates, const auto& ownerTypes) {
        if (candidates.size() == 1) return candidates.front();
        auto site = fn.syntax.expressions[id].span;
        std::vector<OverloadMatch> matches;
        std::vector<std::pair<std::size_t, std::string>> rejected;
        for (auto index : candidates) {
            const auto& item = symbols.callables[index];
            const auto& syntax = symbols.declaration(item);
            Symbols::TypeTrial transaction{symbols};
            auto trial = fn;
            auto returnHint = inferredReturn ? *inferredReturn : std::optional<Type>{};
            Checker checker{trial, source, module, symbols, scopes, variables, loops, workers, catchOwners, lambdaHints, receiverSlot, implicitReceivers, true, inferredReturn ? &returnHint : nullptr, present};
            try {
                const auto& trialCall = std::get<Call>(trial.syntax.expressions[id].node);
                checker.declaredCall(index, id, trialCall, expected, ownerTypes(index), true);
                OverloadMatch match{index, {}, syntax.typeParameters.size() > symbols.enclosingParameters(syntax), !syntax.parameters.empty() && syntax.parameters.back().variadic,
                    trial.expressions[id].omittedDefaults.size()};
                for (const auto& argument : trialCall.arguments) match.arguments.push_back(checker.argumentMatch(argument.value));
                matches.push_back(std::move(match));
            } catch (const Diagnostic& error) {
                rejected.emplace_back(index, error.message);
            }
        }
        if (matches.empty()) {
            auto error = at(source, site, "LYT0019", "没有适用的重载：" + call.name.text);
            for (const auto& [index, reason] : rejected) error.message += "\n  " + symbols.signature(index, call.name.text) + "：" + reason;
            throw error;
        }
        std::vector<const OverloadMatch*> best;
        for (const auto& match : matches) if (std::ranges::none_of(matches, [&](const auto& other) { return other.betterThan(match); })) best.push_back(&match);
        if (best.size() == 1) return best.front()->declaration;
        auto error = at(source, site, "LYT0019", "调用存在重载歧义：" + call.name.text);
        for (const auto* match : best) {
            error.message += "\n  " + symbols.signature(match->declaration, call.name.text);
            std::string conversions;
            for (std::size_t index = 0; index < match->arguments.size(); ++index) if (match->arguments[index].kind != MatchKind::Exact)
                conversions += (conversions.empty() ? "" : "、") + std::to_string(index + 1);
            if (!conversions.empty()) error.message += "，需适配的实参：" + conversions;
        }
        throw error;
    }

    std::size_t selectOverload(ExprId id, const Call& call, std::optional<Type> expected, std::span<const std::size_t> candidates, std::span<const Type> ownerTypes = {}) {
        return selectOverloadWith(id, call, expected, candidates, [&](std::size_t) { return ownerTypes; });
    }

    std::optional<Type> extensionCall(ExprId id, const Call& call, Type receiver, std::optional<Type> expected) {
        std::vector<std::size_t> candidates;
        std::map<std::size_t, std::vector<Type>> receiverTypes;
        for (auto index : symbols.visibleExtensions[fn.syntax.file]) {
            const auto& members = symbols.extensionMethods[index];
            auto found = members.find(call.name.text);
            if (found == members.end()) continue;
            auto types = symbols.extensionTypes(index, receiver);
            if (!types) continue;
            receiverTypes.emplace(index, std::move(*types));
            candidates.insert(candidates.end(), found->second.begin(), found->second.end());
        }
        if (candidates.empty()) return {};
        auto types = [&](std::size_t declaration) -> std::span<const Type> {
            const auto& group = symbols.declaration(symbols.callables[declaration]).extensionGroup;
            return receiverTypes.at(symbols.extensions.at(group->text));
        };
        auto selected = selectOverloadWith(id, call, expected, candidates, types);
        return declaredCall(selected, id, call, expected, types(selected));
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
        if (!call.receiver && !call.typeOwner) if (auto local = localSlot(call.name)) return invoke(readLocal(id, call.name, *local));
        std::size_t declaration;
        std::span<const Type> ownerTypes;
        if (call.typeOwner) {
            const auto& qualifier = *call.typeOwner;
            if (symbols.enums.contains(linkedName(qualifier.name))) return variant(id, qualifier, call, expected);
            auto named = symbols.records.find(linkedName(qualifier.name));
            if (named == symbols.records.end()) throw at(source, qualifier.name.span, "LYT0003", "静态方法需要可见的 Struct 或 Class 类型");
            auto owner = named->second;
            const auto& definition = symbols.recordDeclarations[owner];
            refer(qualifier.name, definition.file, definition.name.span);
            auto member = symbols.staticMethods[owner].find(call.name.text);
            if (member == symbols.staticMethods[owner].end()) {
                if (symbols.methods[owner].contains(call.name.text) || symbols.fields[owner].contains(call.name.text))
                    throw at(source, call.name.span, "LYT0018", "实例成员需要对象接收者：" + call.name.text);
                throw at(source, call.name.span, "LYN0001", "找不到静态方法：" + call.name.text);
            }
            if (!qualifier.arguments.empty()) {
                std::vector<Type> arguments;
                for (const auto& type : qualifier.arguments) arguments.push_back(symbols.resolve(fn.syntax, type, variables));
                auto record = symbols.recordType(owner, std::move(arguments), source, qualifier.name.span);
                ownerTypes = module.aggregates[record.id].arguments;
            } else if (!call.typeArguments.empty() && !symbols.recordDeclarations[owner].typeParameters.empty()) {
                throw at(source, call.name.span, "LYG0001", "显式指定静态方法的类型实参时，先指定所属类型的类型实参，例如 Box<Int>.make<String>(...)");
            }
            auto candidates = symbols.visibleCandidates(member->second, source);
            if (candidates.empty()) throw at(source, call.name.span, "LYN0003", "此成员是模块私有的：" + call.name.text);
            declaration = selectOverload(id, call, expected, candidates, ownerTypes);
        } else if (call.receiver) {
            if (const auto* item = enumNamespace(*call.receiver)) return variant(id, TypeQualifier{*item}, call, expected);
            auto owner = visit(*call.receiver);
            if (owner == Type::Never) return owner;
            bool privateMember = false, staticMember = false;
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
                    info.boundTrait = *symbols.contracts.constraint(owner);
                    bool exits = arguments(id, call, method->parameters.size(), [&](auto slot) -> std::string_view { return method->labels[slot]; }, [&](auto slot) { return method->parameters[slot]; });
                    return exits ? Type{Type::Never} : method->asynchronous ? symbols.task(method->result) : method->result;
                }
            }
            if (owner.kind == Type::Dyn) {
                const auto& trait = module.traits[owner.id];
                if (auto method = symbols.contracts.member(trait.name.text, call.name.text)) {
                    noTypeArgs();
                    auto declaration = std::ranges::find(trait.methods, call.name.text, [](const TraitMethod& candidate) { return candidate.name.text; });
                    refer(call.name, trait.file, declaration->name.span);
                    info.callee = Callee::Dynamic;
                    info.target = declaration - trait.methods.begin();
                    info.interfaceAsync = method->asynchronous;
                    bool exits = arguments(id, call, method->parameters.size(), [&](auto slot) -> std::string_view { return method->labels[slot]; }, [&](auto slot) { return method->parameters[slot]; });
                    return exits ? Type{Type::Never} : method->asynchronous ? symbols.task(method->result) : method->result;
                }
            }
            if (owner.kind == Type::Record) {
                const auto& decl = module.aggregates[owner.id];
                ownerTypes = decl.arguments;
                if (call.boundTrait) {
                    const auto& required = *symbols.contracts.member(*call.boundTrait, call.name.text);
                    auto chosen = symbols.shapedMethod(owner, call.name.text, source, [&](const auto& shape) { return shape == required; });
                    return declaredCall(chosen.value().first, id, call, expected, ownerTypes);
                }
                const auto& fields = symbols.fields[decl.declaration];
                if (auto field = fields.find(call.name.text); field != fields.end()) {
                    visible(decl.syntax.file, decl.syntax.fields[field->second].isPublic, call.name);
                    refer(call.name, decl.syntax.file, decl.syntax.fields[field->second].name.span);
                    info.target = field->second;
                    return invoke(decl.fields[info.target]);
                }
                const auto& methods = symbols.methods[decl.declaration];
                if (auto member = methods.find(call.name.text); member != methods.end()) {
                    auto candidates = symbols.visibleCandidates(member->second, source);
                    if (!candidates.empty()) return declaredCall(selectOverload(id, call, expected, candidates, ownerTypes), id, call, expected, ownerTypes);
                    privateMember = true;
                }
                staticMember = symbols.staticMethods[decl.declaration].contains(call.name.text);
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
            if (auto result = extensionCall(id, call, owner, expected)) return *result;
            if (privateMember) throw at(source, call.name.span, "LYN0003", "此成员是模块私有的：" + call.name.text);
            if (staticMember) throw at(source, call.name.span, "LYT0018", "静态方法必须通过类型名调用：" + call.name.text);
            throw at(source, call.name.span, "LYN0001", typeName(module, owner) + " 没有成员：" + call.name.text);
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
            refer(call.name, decl.file, decl.name.span);
            if (expected && expected->kind == Type::Option) expected = module.sumTypes[expected->id].success;
            if (auto initializer = symbols.initializers[record->second]) {
                declaration = *initializer;
                const auto& ctor = symbols.declaration(symbols.callables[declaration]);
                visible(ctor.file, ctor.isPublic, call.name);
            } else {
                info.callee = Callee::Constructor;
                auto signature = constructorSignature(decl);
                Type type;
                bool exits;
                if (!decl.typeParameters.empty()) {
                    auto bound = bindGeneric(id, call, signature, expected, {}, decl.fields);
                    type = symbols.recordType(record->second, std::move(bound.first), source, call.name.span);
                    exits = bound.second;
                } else {
                    noTypeArgs();
                    type = symbols.recordType(record->second, {}, source, call.name.span);
                    exits = functionArguments(id, call, signature, [&](auto slot, auto, const auto&) { return module.aggregates[type.id].fields[slot]; }, false, decl.fields);
                }
                for (std::size_t index = 0; index < call.arguments.size(); ++index) {
                    const auto& field = decl.fields[info.argumentSlots[index]];
                    visible(decl.file, field.isPublic, call.arguments[index].label.value_or(call.name));
                }
                info.target = type.id;
                if (materializing() && !info.omittedDefaults.empty()) symbols.bindFieldDefaults(type, info.omittedDefaults, source, call.name.span);
                return exits ? Type{Type::Never} : type;
            }
        } else {
            auto candidates = symbols.visibleFunctions(source, call.name);
            if (candidates.empty()) throw at(source, call.name.span, "LYN0001", "找不到函数：" + call.name.text);
            declaration = selectOverload(id, call, expected, candidates);
        }
        return declaredCall(declaration, id, call, expected, ownerTypes);
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
        auto entry = present;
        auto body = visit(worker.body, hint);
        auto result = workers.back().result.value_or(body);
        fit(worker.body, body, result);
        workers.pop_back();
        present = std::move(entry);
        loops = previous;
        return symbols.task(result);
    }

    Type infer(ExprId id, const Deferred& cleanup, auto, bool) {
        auto owner = currentWorker();
        if (std::ranges::find(fn.cleanupOwners, owner) == fn.cleanupOwners.end()) fn.cleanupOwners.push_back(owner);
        workers.push_back({id, fn.locals.size(), Type{Type::Unit}});
        unsigned previous = std::exchange(loops, 0);
        auto entry = present;
        visit(cleanup.body, {}, true);
        present = std::move(entry);
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
            auto entry = present;
            present.merge(definitelyPresent(binary.left, binary.op == Op::And));
            visit(binary.right, Type::Bool);
            present = std::move(entry);
            return type;
        }
        bool equality = binary.op == Op::Eq || binary.op == Op::Ne;
        bool emptyLeft = std::holds_alternative<EmptyOption>(fn.syntax.expressions[binary.left].node);
        bool emptyRight = std::holds_alternative<EmptyOption>(fn.syntax.expressions[binary.right].node);
        if (equality && (emptyLeft || emptyRight)) {
            auto other = inspectOption(emptyLeft ? binary.right : binary.left);
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
        if (equality && (left.kind == Type::Option || left.kind == Type::Result || left.kind == Type::Enum || left.kind == Type::Closure || left.kind == Type::Any || left.kind == Type::Dyn || left.kind == Type::Variable || (left.kind == Type::Record && !module.aggregates[left.id].syntax.reference))) throw at(source, binary.site, "LYT0006", typeName(module, left) + " 未提供 Eq，不能用 == 或 != 比较");
        if (binary.op == Op::Add && left == Type::String) return Type::String;
        if (!equality) throw mismatch(module, source, binary.site, Type::Int, left);
        return Type::Bool;
    }

    Type infer(ExprId id, const Block& block, std::optional<Type> expected, bool discard) {
        if (id != fn.syntax.body) scopes.emplace_back();
        auto returnHint = id == fn.syntax.body ? inferredReturn : nullptr;
        bool live = true;
        Type type = Type::Unit;
        Span site = block.end;
        bool semiFix = false;
        for (const auto& statement : block.statements) {
            bool tail = !discard && &statement == &block.statements.back() && !statement.semicolon && live;
            auto wanted = returnHint ? *returnHint : expected;
            auto value = visit(statement.expression, tail ? wanted : std::nullopt, !tail);
            live &= value != Type::Never;
            type = statement.semicolon ? Type::Unit : value;
            site = statement.semicolon.value_or(fn.syntax.expressions[statement.expression].span);
            semiFix = statement.semicolon && expected == value;
        }
        if (id != fn.syntax.body) scopes.pop_back();
        if (!live) return Type::Never;
        if (returnHint) expected = *returnHint;
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
        auto entry = present;
        present.merge(definitelyPresent(branch.condition, true));
        auto yes = visit(branch.yes, ignore ? std::nullopt : expected, ignore);
        auto yesFacts = std::exchange(present, std::move(entry));
        present.merge(definitelyPresent(branch.condition, false));
        auto no = branch.no ? visit(*branch.no, !ignore && yes != Type::Never ? std::optional(yes) : expected, ignore) : Type::Unit;
        if (condition == Type::Never || (yes == Type::Never && no == Type::Never)) return Type::Never;
        if (no == Type::Never) present = std::move(yesFacts);
        else if (yes != Type::Never) present = bothPaths(present, yesFacts);
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
        if (!target && value != Type::Never) {
            if (workers.empty()) *inferredReturn = value;
            else workers.back().result = value;
        }
        return Type::Never;
    }

    Type infer(ExprId, const While& loop, auto, bool) {
        auto condition = visit(loop.condition, Type::Bool);
        auto entry = present;
        present.merge(definitelyPresent(loop.condition, true));
        ++loops;
        visit(loop.body, {}, true);
        --loops;
        present = std::move(entry);
        return condition == Type::Never ? Type::Never : Type::Unit;
    }

    struct IterationMethod { Type result; std::optional<std::size_t> function; };

    IterationMethod iterationMethod(Type owner, const std::string& name, Span site) {
        if (owner.kind == Type::Variable) {
            if (const auto* method = symbols.contracts.member(owner, name); method && method->parameters.empty() && !method->asynchronous)
                return {method->result, {}};
        } else if (owner.kind == Type::Record) {
            const auto& record = module.aggregates[owner.id];
            auto selected = symbols.shapedMethod(owner, name, source, [](const auto& shape) { return shape.parameters.empty() && !shape.asynchronous; });
            if (selected) {
                const auto& entry = symbols.callables[selected->first];
                std::optional<std::size_t> function;
                if (materializing()) function = entry.generic ? symbols.instantiate(selected->first, record.arguments, source, site) : entry.target;
                return {selected->second.result, function};
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
        fn.locals.push_back({element, false, currentWorker()});
        declare(loop.item, slot);
        fn.expressions[id].target = slot;
        auto entry = present;
        ++loops;
        visit(loop.body, {}, true);
        --loops;
        present = std::move(entry);
        scopes.pop_back();
        return exits ? Type::Never : Type::Unit;
    }

    Type infer(ExprId id, Jump, auto, bool) {
        if (!loops) throw at(source, fn.syntax.expressions[id].span, "LYT0005", "break 和 continue 只可用于循环体");
        return Type::Never;
    }
};

const Identifier& Symbols::resultAnnotation(const Callable& candidate, const Source& use, Span site) {
    const auto& syntax = declaration(candidate);
    if (!syntax.result.text.empty()) return syntax.result;
    if (candidate.inferredResult) return *candidate.inferredResult;
    if (candidate.inferring) throw at(use, site, "LYT0020", "递归函数需要标注返回类型：" + declarationSite(syntax));
    candidate.inferring = true;
    try {
        auto body = prepare(syntax, candidate.variables);
        auto type = Checker{body, sources[syntax.file], module, *this, {}, candidate.variables}.inferResult();
        candidate.inferredResult = Identifier{typeName(module, type, true), syntax.result.span};
    } catch (...) {
        candidate.inferring = false;
        throw;
    }
    candidate.inferring = false;
    return *candidate.inferredResult;
}

}

CheckedModule check(Module module, const std::vector<Source>& sources, bool requireEntry) {
    if (!module.expansions.empty()) {
        auto& use = module.expansions.front();
        throw at(sources[use.file], use.name.span, "LYM0001", "声明前缀未展开：" + use.name.text);
    }
    CheckedModule checked{};
    Symbols symbols{checked, sources, {}, {}, {}, {}, {}, {}};
    auto expose = [&](Type type, const Source& source, Span site) {
        if (auto hidden = hiddenType(checked, type)) throw at(source, site, "LYN0003", "公开签名不能暴露模块私有类型：" + typeName(checked, *hidden));
    };
    auto exposeBounds = [&](const auto& syntax) {
        for (const auto& bound : syntax.bounds) {
            const auto& trait = *std::ranges::find(module.traits, bound.trait.text, [](const Trait& item) { return item.name.text; });
            if (!trait.isPublic) throw at(sources[syntax.file], bound.trait.span, "LYN0003", "公开签名不能暴露模块私有约束：" + bound.trait.text);
        }
    };
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
        reserveType(decl, symbols.enums, symbols.enumDeclarations.size());
        symbols.enumDeclarations.push_back(std::move(decl));
    }
    symbols.fields.resize(symbols.recordDeclarations.size());
    symbols.methods.resize(symbols.recordDeclarations.size());
    symbols.staticMethods.resize(symbols.recordDeclarations.size());
    symbols.initializers.resize(symbols.recordDeclarations.size());
    symbols.variants.resize(symbols.enumDeclarations.size());
    symbols.extensionDeclarations = std::move(module.extensions);
    symbols.extensionMethods.resize(symbols.extensionDeclarations.size());
    symbols.visibleExtensions.resize(sources.size());
    for (std::size_t index = 0; index < symbols.extensionDeclarations.size(); ++index) {
        const auto& group = symbols.extensionDeclarations[index];
        reserveType(group, symbols.extensions, index);
    }
    if (!symbols.extensionDeclarations.empty()) for (std::size_t file = 0; file < sources.size(); ++file) {
        std::set<std::size_t> visible;
        if (!module.importedExtensions.empty()) for (const auto& group : module.importedExtensions[file]) visible.insert(symbols.extensions.at(group));
        for (std::size_t index = 0; index < symbols.extensionDeclarations.size(); ++index)
            if (sources[symbols.extensionDeclarations[index].file].moduleId == sources[file].moduleId) visible.insert(index);
        symbols.visibleExtensions[file].assign(visible.begin(), visible.end());
    }
    for (const auto& trait : module.traits) {
        const auto& source = sources[trait.file];
        if (symbols.records.contains(trait.name.text) || symbols.enums.contains(trait.name.text)) throw at(source, trait.name.span, "LYN0002", "类型名称已声明：" + trait.name.text);
        symbols.contracts.declare(trait, source);
        symbols.traitNames.emplace(trait.name.text, checked.traits.size());
        checked.traits.push_back(trait);
    }
    auto typeVariables = [&](const auto& syntax) {
        std::set<std::string> seen;
        std::vector<Type> variables;
        for (const auto& parameter : syntax.typeParameters) {
            if (!seen.insert(parameter.text).second) throw at(sources[syntax.file], parameter.span, "LYN0002", "类型形参重复：" + parameter.text);
            variables.emplace_back(Type::Variable, checked.variableNames.size());
            checked.variableNames.push_back(parameter.text);
        }
        for (const auto& bound : syntax.bounds) symbols.contracts.bind(variables[bound.parameter], bound.trait, sources[syntax.file]);
        return variables;
    };
    for (std::size_t id = 0; id < symbols.recordDeclarations.size(); ++id) {
        const auto& syntax = symbols.recordDeclarations[id];
        const auto& source = sources[syntax.file];
        for (std::size_t slot = 0; slot < syntax.fields.size(); ++slot) {
            const auto& field = syntax.fields[slot];
            if (!symbols.fields[id].emplace(field.name.text, slot).second) throw at(source, field.name.span, "LYN0002", "字段重复：" + field.name.text);
        }
        auto type = symbols.recordType(id, typeVariables(syntax), source, syntax.name.span);
        if (syntax.isPublic) {
            exposeBounds(syntax);
            for (std::size_t slot = 0; slot < syntax.fields.size(); ++slot) if (syntax.fields[slot].isPublic)
                expose(checked.aggregates[type.id].fields[slot], source, syntax.fields[slot].type.span);
        }
    }
    for (const auto& trait : module.traits) {
        const auto& source = sources[trait.file];
        symbols.contracts.define(trait, source, [&](const Identifier& type) {
            auto resolved = symbols.resolve(source, type);
            if (trait.isPublic) expose(resolved, source, type.span);
            return resolved;
        });
    }
    for (std::size_t id = 0; id < symbols.enumDeclarations.size(); ++id) {
        const auto& syntax = symbols.enumDeclarations[id];
        const auto& source = sources[syntax.file];
        for (std::size_t index = 0; index < syntax.cases.size(); ++index) {
            const auto& entry = syntax.cases[index];
            if (!symbols.variants[id].emplace(entry.name.text, index).second) throw at(source, entry.name.span, "LYN0002", "枚举分支重复：" + entry.name.text);
            std::set<std::string> labels;
            for (const auto& field : entry.fields) {
                if (field.name && !labels.insert(field.name->text).second) throw at(source, field.name->span, "LYN0002", "枚举分支载荷名重复：" + field.name->text);
            }
        }
        auto type = symbols.enumType(id, typeVariables(syntax), source, syntax.name.span);
        if (syntax.isPublic) {
            exposeBounds(syntax);
            for (std::size_t index = 0; index < syntax.cases.size(); ++index)
                for (std::size_t slot = 0; slot < syntax.cases[index].fields.size(); ++slot)
                    expose(checked.enums[type.id].payloads[index][slot], source, syntax.cases[index].fields[slot].type.span);
        }
    }
    for (const auto& group : symbols.extensionDeclarations) {
        const auto& source = sources[group.file];
        auto form = typeForm(group.receiver.text);
        std::set<std::string> used;
        auto scan = [&](auto&& self, const TypeForm& nested) -> void {
            if (nested.arguments.empty()) used.insert(nested.name);
            for (const auto& part : nested.arguments) self(self, part);
        };
        scan(scan, form);
        for (const auto& parameter : group.typeParameters) if (!used.contains(parameter.text))
            throw at(source, parameter.span, "LYG0002", "扩展组类型形参必须出现在接收者类型中：" + parameter.text);
        auto variables = typeVariables(group);
        auto receiver = symbols.resolveWith(form, source, group.receiver.span, group.typeParameters, [&](auto slot) { return variables[slot]; }).value();
        if (group.isPublic) {
            exposeBounds(group);
            expose(receiver, source, group.receiver.span);
        }
    }
    auto exposeFunction = [&](const CheckedFunction& function) {
        const auto& syntax = function.syntax;
        if (!syntax.isPublic || syntax.conversion) return;
        if (syntax.owner && !symbols.recordDeclarations[symbols.records.at(syntax.owner->text)].isPublic) return;
        const auto& source = sources[syntax.file];
        exposeBounds(syntax);
        expose(function.result, source, syntax.result.span);
        for (auto slot = static_cast<std::size_t>(syntax.hasReceiver); slot < syntax.parameters.size(); ++slot)
            expose(function.locals[slot].type, source, syntax.parameters[slot].type.span);
    };
    auto addOverload = [&](std::vector<std::size_t>& candidates, const Function& function, const Source& source) {
        auto signature = overloadKey(function);
        for (auto index : candidates) {
            const auto& prior = symbols.declaration(symbols.callables[index]);
            if (overloadKey(prior) == signature) throw at(source, function.name.span, "LYN0002", "函数重载的形参类型重复：" + symbols.declarationSite(prior));
        }
        candidates.push_back(symbols.callables.size());
    };
    for (auto& function : module.functions) {
        const auto& source = sources[function.file];
        auto owner = function.owner ? std::optional(symbols.records.at(function.owner->text)) : std::nullopt;
        auto group = function.extensionGroup ? std::optional(symbols.extensions.at(function.extensionGroup->text)) : std::nullopt;
        if (function.initializer) {
            if (symbols.initializers[*owner]) throw at(source, function.name.span, "LYN0002", "class 只允许一个 init");
            symbols.initializers[*owner] = symbols.callables.size();
        }
        else if (group) addOverload(symbols.extensionMethods[*group][function.name.text], function, source);
        else if (owner) {
            auto& names = function.hasReceiver ? symbols.methods[*owner] : symbols.staticMethods[*owner];
            if (function.hasReceiver && symbols.fields[*owner].contains(function.name.text))
                throw at(source, function.name.span, "LYN0002", "函数名已声明：" + function.name.text);
            addOverload(names[function.name.text], function, source);
        } else {
            const auto& name = function.name.text;
            if (symbols.records.contains(name) || symbols.enums.contains(name) || name == "print" || name == "gcCollect")
                throw at(source, function.name.span, "LYN0002", "函数名已声明：" + name);
            auto& candidates = symbols.functions[name];
            if (!candidates.empty() && (name == "main" || name.ends_with("::main"))) throw at(source, function.name.span, "LYN0002", "main 不能重载");
            addOverload(candidates, function, source);
        }
        if (!function.typeParameters.empty()) {
            if (!owner && !group && requireEntry && function.name.text == "main") throw at(source, function.name.span, "LYG0001", "main 不能声明类型形参");
            auto variables = typeVariables(function);
            symbols.callables.push_back({0, std::move(function), std::move(variables)});
            continue;
        }
        auto concrete = symbols.prepare(std::move(function));
        exposeFunction(concrete);
        const auto& syntax = concrete.syntax;
        if (!owner && !group && requireEntry && syntax.name.text == "main") {
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
    symbols.boundsReady = true;
    symbols.validateNominalBounds();
    using FunctionSite = std::pair<std::size_t, std::size_t>;
    std::map<FunctionSite, std::vector<FunctionSite>> dependencies;
    auto rememberReferences = [&](const CheckedFunction& fn) {
        checked.references.insert(checked.references.end(), fn.references.begin(), fn.references.end());
        auto& calls = dependencies[{fn.syntax.file, fn.syntax.name.span.begin}];
        for (const auto& call : fn.calls) calls.emplace_back(call.file, call.span.begin);
    };
    for (const auto& callable : symbols.callables) {
        const auto& syntax = symbols.declaration(callable);
        if (!syntax.result.text.empty()) continue;
        const auto& annotation = symbols.resultAnnotation(callable, sources[syntax.file], syntax.name.span);
        if (!callable.generic) checked.functions[callable.target].result = symbols.resolve(syntax, annotation, {});
    }
    for (auto& callable : symbols.callables) if (callable.generic) {
        auto rememberContracts = [&](const CheckedFunction& fn) {
            for (ExprId id = 0; id < callable.generic->expressions.size(); ++id) {
                if (const auto& bound = fn.expressions[id].boundTrait)
                    std::get<Call>(callable.generic->expressions[id].node).boundTrait = bound;
            }
        };
        for (std::size_t slot = 0; slot < callable.generic->parameters.size(); ++slot) if (callable.generic->parameters[slot].defaultValue) {
            auto helper = symbols.prepare(defaultFunction(*callable.generic, slot), callable.variables);
            Checker{helper, sources[helper.syntax.file], checked, symbols, {}, callable.variables}.run();
            rememberContracts(helper);
            rememberReferences(helper);
        }
        auto syntax = *callable.generic;
        syntax.result = symbols.resultAnnotation(callable, sources[syntax.file], syntax.name.span);
        auto body = symbols.prepare(std::move(syntax), callable.variables);
        exposeFunction(body);
        Checker{body, sources[body.syntax.file], checked, symbols, {}, callable.variables}.run();
        rememberContracts(body);
        rememberReferences(body);
    }
    for (std::size_t index = 0; index < checked.functions.size(); ++index) {
        auto& fn = checked.functions[index];
        Checker{fn, sources[fn.syntax.file], checked, symbols, {}}.run();
        rememberReferences(fn);
    }
    for (const auto& callable : symbols.callables) if (callable.inferredResult) {
        const auto& syntax = symbols.declaration(callable);
        FunctionSite root{syntax.file, syntax.name.span.begin};
        std::set<FunctionSite> visited{root};
        auto work = dependencies.at(root);
        for (std::size_t index = 0; index < work.size(); ++index) {
            auto target = work[index];
            if (target == root) throw at(sources[syntax.file], syntax.name.span, "LYT0020", "递归函数需要标注返回类型");
            if (visited.insert(target).second) if (auto next = dependencies.find(target); next != dependencies.end())
                work.insert(work.end(), next->second.begin(), next->second.end());
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
    validateForeignDeclarations(checked, sources);
    if (requireEntry && !checked.entry) throw Diagnostic{"LYN0001", "可执行包缺少入口 fn main(): Unit"};
    return checked;
}

}
