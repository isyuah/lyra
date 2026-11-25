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
import lyra.ctfe;

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
        case Type::TaskLocal: return hiddenType(module, module.taskLocalValues[type.id]);
        case Type::Channel: return hiddenType(module, module.channelElements[type.id]);
        case Type::Option: return hiddenType(module, module.sumTypes[type.id].success);
        case Type::Result: {
            const auto& sum = module.sumTypes[type.id];
            if (auto hidden = hiddenType(module, sum.success)) return hidden;
            return hiddenType(module, sum.failure);
        }
        case Type::Dyn: return module.traits[type.id].isPublic ? std::nullopt : std::optional(type);
        case Type::Exception: return type.id < firstDeclaredException || module.exceptions[type.id - firstDeclaredException].isPublic ? std::nullopt : std::optional(type);
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

constexpr Type scalarTypes[] = {Type::Bool, Type::String, Type::Char, Type::Bytes, Type::Unit, Type::CPtr, Type::Exception, channelClosed, Type::Any, Type::Error};

bool builtInDisplay(Type type) { return numericType(type) || type == Type::Bool || type == Type::String || type == Type::Char; }

std::string typeName(const CheckedModule& module, Type type, bool canonical = false) {
    auto name = [&](Type nested) { return typeName(module, nested, canonical); };
    switch (type.kind) {
        case Type::Bool: return "Bool";
        case Type::Int: case Type::UInt: case Type::Real: return numberName(type);
        case Type::Unit: return "Unit";
        case Type::Never: return canonical ? "$never" : "Never";
        case Type::String: return "String";
        case Type::Char: return "Char";
        case Type::Bytes: return "Bytes";
        case Type::Exception: {
            if (!type.id) return "Exception";
            if (type == channelClosed) return "ChannelClosed";
            const auto& full = module.exceptions[type.id - firstDeclaredException].name.text;
            auto separator = full.rfind("::");
            return canonical || separator == full.npos ? full : full.substr(separator + 2);
        }
        case Type::CPtr: return "CPtr";
        case Type::Any: return "Any";
        case Type::Error: return "Error";
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
        case Type::Projection: {
            const auto& projection = module.projections[type.id];
            return canonical ? "$assoc:" + projection.member + '<' + name(projection.owner) + '>' : name(projection.owner) + '.' + projection.member;
        }
        case Type::Task: return "Task<" + name(module.taskResults[type.id]) + ">";
        case Type::TaskLocal: return "TaskLocal<" + name(module.taskLocalValues[type.id]) + ">";
        case Type::Channel: return "Channel<" + name(module.channelElements[type.id]) + ">";
        case Type::List: return "List<" + name(module.listElements[type.id]) + ">";
        case Type::IntRange: return "Range<Int>";
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

struct NoTypeLookup {
    std::optional<Type> operator()(std::size_t) const { return {}; }
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

enum class MatchKind { Exact, Number, Nullable, Literal, Upcast, User, Box };
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
    syntax.throwsTypes.clear();
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
    std::map<std::pair<Type, std::string>, std::size_t> builtinImplementations{};
    std::vector<DirectConversion> conversions{};
    StructuralContracts contracts{};
    Names traitNames{};
    Names exceptionTypes{};
    Names enums{};
    std::vector<Names> variants{};
    std::vector<Enumeration> enumDeclarations{};
    std::vector<Aggregate> recordDeclarations{};
    std::vector<std::optional<std::size_t>> initializers{};
    std::vector<Overloads> staticMethods{};
    Names extensions{};
    std::vector<Extension> extensionDeclarations{};
    std::vector<Overloads> extensionMethods{};
    std::vector<Names> extensionProperties{};
    std::vector<std::size_t> functionCallables{};
    std::vector<std::vector<std::size_t>> visibleExtensions{};
    std::map<std::pair<std::size_t, std::vector<Type>>, Type> recordInstances{};
    std::map<std::pair<std::size_t, std::vector<Type>>, Type> enumInstances{};
    std::vector<std::pair<bool, std::size_t>> activeNominals{};
    unsigned expandingNominal{};
    struct NominalCheck { Type type; const Source* source; Span site; };
    std::vector<NominalCheck> nominalChecks{};
    std::size_t nextNominalCheck{};
    bool boundsReady{};
    std::vector<std::pair<Type, std::string>> resolvingAssociated{};
    Names aliases{};
    std::vector<TypeAlias> aliasDeclarations{};
    std::vector<std::string> resolvingAliases{};

    struct TypeTrial {
        Symbols& symbols;
        std::size_t aggregates = symbols.module.aggregates.size(), enums = symbols.module.enums.size(), tasks = symbols.module.taskResults.size(),
            sums = symbols.module.sumTypes.size(), lists = symbols.module.listElements.size(), signatures = symbols.module.signatures.size(),
            taskLocals = symbols.module.taskLocalValues.size(), channels = symbols.module.channelElements.size(), projections = symbols.module.projections.size(),
            deferredPatterns = symbols.module.deferredPatternChecks.size();
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
            symbols.module.taskLocalValues.resize(taskLocals);
            symbols.module.channelElements.resize(channels);
            symbols.module.sumTypes.resize(sums);
            symbols.module.listElements.resize(lists);
            symbols.module.signatures.resize(signatures);
            symbols.module.projections.resize(projections);
            symbols.module.deferredPatternChecks.resize(deferredPatterns);
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

    std::optional<std::size_t> valueOverride(Type type, ValueOperation operation) {
        if (type.kind != Type::Record || module.aggregates[type.id].syntax.reference) return {};
        const auto& record = module.aggregates[type.id];
        auto name = operation == ValueOperation::Equal ? "equals" : "hashCode";
        const auto& members = methods[record.declaration];
        auto candidates = members.find(name);
        if (candidates == members.end()) return {};
        for (auto index : candidates->second) {
            const auto& candidate = declaration(callables[index]);
            if (candidate.result.text.empty() || candidate.isAsync || candidate.typeParameters.size() != record.arguments.size()) continue;
            bool equality = operation == ValueOperation::Equal;
            if (candidate.parameters.size() != (equality ? 2u : 1u)) continue;
            if (equality && (candidate.parameters[1].variadic || resolve(candidate, candidate.parameters[1].type, record.arguments) != type)) continue;
            if (resolve(candidate, candidate.result, record.arguments) == (equality ? Type::Bool : Type::Int)) return index;
        }
        return {};
    }

    struct ValueSupport {
        std::optional<std::string> missing{};
        std::vector<std::pair<Type, std::size_t>> overrides{};
    };

    ValueSupport valueSupport(Type root, ValueOperation operation) {
        ValueSupport result;
        std::set<Type> seen;
        std::string path;
        const auto equality = operation == ValueOperation::Equal;
        auto check = [&](auto&& self, Type type) -> bool {
            if (!seen.insert(type).second) return true;
            auto reject = [&](std::string reason) {
                result.missing = (path.empty() ? "" : path + "：") + std::move(reason);
                return false;
            };
            auto child = [&](Type part, const std::string& name) {
                auto length = path.size();
                path += '.' + name;
                bool supported = self(self, part);
                path.resize(length);
                return supported;
            };
            if (auto implementation = valueOverride(type, operation)) {
                if (!equality && !valueOverride(type, ValueOperation::Equal))
                    return reject(typeName(module, type) + " 手写了 hashCode，需要配套的 equals 才能满足 Hash");
                result.overrides.emplace_back(type, *implementation);
                return true;
            }
            switch (type.kind) {
                case Type::Int: case Type::UInt: case Type::Real: case Type::Bool: case Type::Char:
                case Type::String: case Type::Bytes: case Type::Unit: case Type::IntRange: return true;
                case Type::Variable:
                    if (std::ranges::find(contracts.constraints(type), equality ? "Eq" : "Hash") != contracts.constraints(type).end()) return true;
                    break;
                case Type::Record: {
                    const auto& record = module.aggregates[type.id];
                    if (record.syntax.reference) {
                        return equality || reject(typeName(module, type) + " 按身份比较，但没有默认 Hash；请使用显式哈希回调");
                    }
                    if (!equality && valueOverride(type, ValueOperation::Equal))
                        return reject(typeName(module, type) + " 手写了 equals，需要配套的 hashCode 才能满足 Hash");
                    for (std::size_t slot = 0; slot < record.fields.size(); ++slot) {
                        if (!child(record.fields[slot], record.syntax.fields[slot].name.text)) return false;
                    }
                    return true;
                }
                case Type::Enum: {
                    const auto& item = module.enums[type.id];
                    for (std::size_t variant = 0; variant < item.payloads.size(); ++variant) {
                        const auto& entry = item.syntax.cases[variant];
                        for (std::size_t slot = 0; slot < item.payloads[variant].size(); ++slot) {
                            const auto& field = entry.fields[slot];
                            auto name = field.name ? field.name->text : std::to_string(slot + 1);
                            if (!child(item.payloads[variant][slot], entry.name.text + '.' + name)) return false;
                        }
                    }
                    return true;
                }
                case Type::Option: return child(module.sumTypes[type.id].success, "Some");
                case Type::Result:
                    return child(module.sumTypes[type.id].success, "Ok") && child(module.sumTypes[type.id].failure, "Err");
                case Type::Task: case Type::List: case Type::CPtr:
                case Type::TaskLocal: case Type::Exception: case Type::Channel:
                    if (equality) return true;
                    break;
                default: break;
            }
            return reject(typeName(module, type) + (equality ? " 未提供 Eq" : " 未提供 Hash"));
        };
        check(check, root);
        return result;
    }

    std::optional<std::string> valuePairMismatch(Type root) {
        std::vector<Type> pending{root};
        std::set<Type> seen;
        while (!pending.empty()) {
            auto type = pending.back();
            pending.pop_back();
            if (!seen.insert(type).second) continue;
            if (type.kind == Type::Record) {
                const auto& record = module.aggregates[type.id];
                auto equality = valueOverride(type, ValueOperation::Equal).has_value();
                auto hashing = valueOverride(type, ValueOperation::Hash).has_value();
                if (equality != hashing) return typeName(module, type) + " 的 equals 与 hashCode 必须同时提供，才能用于要求 Eq + Hash 的键";
                if (!equality) pending.insert(pending.end(), record.fields.begin(), record.fields.end());
            } else if (type.kind == Type::Enum) {
                for (const auto& fields : module.enums[type.id].payloads) pending.insert(pending.end(), fields.begin(), fields.end());
            } else if (type.kind == Type::Option || type.kind == Type::Result) {
                const auto& shape = module.sumTypes[type.id];
                pending.push_back(shape.success);
                if (type.kind == Type::Result) pending.push_back(shape.failure);
            }
        }
        return {};
    }

    void bindValueOperations(ValueOperation operation, const ValueSupport& support, const Source& use, Span site) {
        for (auto [type, index] : support.overrides) {
            auto key = std::pair{type, operation};
            if (module.valueOverrides.contains(key)) continue;
            const auto& target = callables[index];
            auto function = target.generic ? instantiate(index, module.aggregates[type.id].arguments, use, site) : target.target;
            module.valueOverrides.emplace(key, function);
        }
    }

    struct BuiltinMember { Callee callee; RequiredMethod shape; };

    std::optional<BuiltinMember> builtinMember(Type type, std::string_view name) {
        auto noArgs = [](Callee callee, Type result) { return BuiltinMember{callee, {{}, {}, result, false}}; };
        if (name == "size" && (type.kind == Type::List || type == Type::String || type == Type::Bytes)) return noArgs(Callee::Size, Type::Int);
        if (name == "add" && type.kind == Type::List) return BuiltinMember{Callee::ListAdd, {{"value"}, {module.listElements[type.id]}, Type::Unit, false}};
        if (name == "slice" && type.kind == Type::List) return BuiltinMember{Callee::ListSlice, {{"range"}, {Type::IntRange}, type, false}};
        if (type.kind == Type::Channel) {
            auto element = module.channelElements[type.id];
            if (name == "send") return BuiltinMember{Callee::ChannelSend, {{"value"}, {element}, task(Type::Unit), false}};
            if (name == "receive") return noArgs(Callee::ChannelReceive, task(sum(Type::Option, element)));
            if (name == "close") return noArgs(Callee::ChannelClose, Type::Unit);
        }
        if (name == "toBytes" && type == Type::String) return noArgs(Callee::ToBytes, Type::Bytes);
        if (name == "toString") {
            if (builtInDisplay(type)) return noArgs(Callee::ToString, Type::String);
            if (type == Type::Bytes) return noArgs(Callee::DecodeUtf8, sum(Type::Option, Type::String));
        }
        if (numericType(type) && name.starts_with("to")) {
            name.remove_prefix(2);
            bool checked = name.ends_with("Checked");
            if (checked) name.remove_suffix(7);
            if (auto target = numberType(name); target && (checked ? integerType(*target) : target->kind == Type::Real))
                return checked ? noArgs(Callee::ToIntegerChecked, sum(Type::Option, *target)) : noArgs(Callee::ToReal, *target);
        }
        return {};
    }

    std::size_t builtinImplementation(Type owner, const std::string& name, const BuiltinMember& builtin, std::size_t file, Span site) {
        auto [entry, inserted] = builtinImplementations.try_emplace({owner, name}, module.functions.size());
        if (!inserted) return entry->second;
        auto annotation = [&](Type type) { return Identifier{typeName(module, type, true), site}; };
        Identifier receiver{"$receiver", site};
        std::vector<Parameter> parameters{{receiver, annotation(owner)}};
        std::vector<Expr> body{{site, receiver}};
        Call call{{name, site}, {}, 0};
        for (auto type : builtin.shape.parameters) {
            Identifier parameter{"$arg" + std::to_string(parameters.size()), site};
            parameters.push_back({parameter, annotation(type)});
            call.arguments.push_back({{}, body.size()});
            body.push_back({site, std::move(parameter)});
        }
        auto tail = body.size();
        body.push_back({site, std::move(call)});
        addConcrete(prepare({file, {"$builtin$" + name, site}, std::move(parameters), annotation(builtin.shape.result), std::move(body), tail}));
        return entry->second;
    }

    RequiredMethod requiredShape(Type owner, const Trait& trait, const TraitMethod& method, const Source& use, Span site) {
        auto resolve = [&](const Identifier& annotation) {
            return resolveWith(typeForm(annotation.text), sources[trait.file], annotation.span, trait.associated, [&](std::size_t slot) {
                return projectionType(owner, trait.associated[slot].text, use, site);
            }).value();
        };
        RequiredMethod shape{{}, {}, resolve(method.result), method.asynchronous};
        for (const auto& parameter : method.parameters) {
            shape.labels.push_back(parameter.name.text);
            shape.parameters.push_back(resolve(parameter.type));
        }
        return shape;
    }

    struct BoundMethod { const Trait* trait; const TraitMethod* method; RequiredMethod shape; };

    std::optional<BoundMethod> boundMember(Type owner, const std::string& name, const Source& use, Span site) {
        std::optional<BoundMethod> result;
        for (const auto& bound : contracts.constraints(owner)) {
            if (valueConstraint(bound)) continue;
            const auto& trait = contracts.declaration(bound);
            auto method = std::ranges::find(trait.methods, name, [](const auto& entry) { return entry.name.text; });
            if (method == trait.methods.end()) continue;
            auto shape = requiredShape(owner, trait, *method, use, site);
            if (!result) result = BoundMethod{&trait, &*method, std::move(shape)};
            else if (result->shape != shape)
                throw at(use, site, "LYG0006", "结构约束 " + result->trait->name.text + " 和 " + bound + " 为成员提供了冲突签名：" + name);
        }
        return result;
    }

    void requireBounds(std::span<const Bound> bounds, std::size_t file, std::span<const Type> types, const Source& use, Span site) {
        const auto& definition = sources[file];
        auto nameOf = [&](Type type) { return typeName(module, type); };
        for (const auto& bound : bounds) {
            if (bound.trait.text != "Eq" || types[bound.parameter].kind != Type::Record) continue;
            if (!std::ranges::any_of(bounds, [&](const Bound& other) { return other.parameter == bound.parameter && other.trait.text == "Hash"; })) continue;
            if (auto mismatch = valuePairMismatch(types[bound.parameter]))
                throw at(use, site, "LYG0005", nameOf(types[bound.parameter]) + " 不满足 Eq + Hash：" + *mismatch);
        }
        for (const auto& bound : bounds) {
            auto actual = types[bound.parameter];
            if (auto operation = valueConstraint(bound.trait.text)) {
                if (auto support = valueSupport(actual, *operation); support.missing)
                    throw at(use, site, "LYG0005", nameOf(actual) + " 不满足 " + bound.trait.text + "：" + *support.missing);
                continue;
            }
            const auto& trait = contracts.declaration(bound.trait.text);
            auto fail = [&](const std::string& reason) { return at(use, site, "LYG0005", nameOf(actual) + " 不满足 " + trait.name.text + "：" + reason); };
            for (const auto& member : trait.associated)
                if (!associatedType(actual, member.text, definition, site)) throw fail("缺少可见关联类型 " + member.text);
            for (const auto& method : trait.methods) {
                const auto& name = method.name.text;
                auto expected = requiredShape(actual, trait, method, definition, site);
                std::optional<RequiredMethod> found;
                auto candidate = [&](const Trait& provided) {
                    auto member = std::ranges::find(provided.methods, name, [](const auto& entry) { return entry.name.text; });
                    if (member == provided.methods.end()) return false;
                    auto shape = requiredShape(actual, provided, *member, definition, site);
                    if (shape == expected) return true;
                    found = std::move(shape);
                    return false;
                };
                if (actual.kind == Type::Variable) {
                    if (std::ranges::any_of(contracts.constraints(actual), [&](const auto& available) { return !valueConstraint(available) && candidate(contracts.declaration(available)); })) continue;
                } else if (actual.kind == Type::Dyn) {
                    if (candidate(module.traits[actual.id])) continue;
                } else {
                    if (auto builtin = builtinMember(actual, name)) {
                        if (builtin->shape == expected) continue;
                        found = std::move(builtin->shape);
                    }
                    if (shapedMethod(actual, name, definition, [&](const auto& shape) { return shape == expected; })) continue;
                    if (auto available = shapedMethod(actual, name, definition, [](const auto&) { return true; })) found = std::move(available->second);
                }
                auto reason = found ? "，实际为 " + describeRequirement(name, *found, nameOf) : "，缺少可见方法 " + name;
                throw fail("需要 " + describeRequirement(name, expected, nameOf) + reason);
            }
        }
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
    Type taskLocal(Type value) const { return unaryType(Type::TaskLocal, module.taskLocalValues, value); }
    Type channel(Type element) const { return unaryType(Type::Channel, module.channelElements, element); }
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
        if (name == "TaskLocal" && value.kind == Type::TaskLocal && slot == 0) return module.taskLocalValues[value.id];
        if (name == "Channel" && value.kind == Type::Channel && slot == 0) return module.channelElements[value.id];
        if (name == "List" && value.kind == Type::List && slot == 0) return module.listElements[value.id];
        if (name == "Range" && value == Type::IntRange && slot == 0) return Type::Int;
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
            case Type::Variable: case Type::Projection: return true;
            case Type::Record: return module.aggregates[type.id].abstract;
            case Type::Enum: return module.enums[type.id].abstract;
            case Type::Task: return abstract(module.taskResults[type.id]);
            case Type::TaskLocal: return abstract(module.taskLocalValues[type.id]);
            case Type::Channel: return abstract(module.channelElements[type.id]);
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

    std::optional<Type> associatedType(Type owner, const std::string& member, const Source& source, Span site) {
        if (owner.kind == Type::Variable) {
            for (const auto& bound : contracts.constraints(owner)) {
                if (valueConstraint(bound)) continue;
                const auto& members = contracts.declaration(bound).associated;
                if (std::ranges::find(members, member, &Identifier::text) == members.end()) continue;
                TypeProjection projection{owner, member};
                auto found = std::ranges::find(module.projections, projection);
                auto id = static_cast<std::size_t>(found - module.projections.begin());
                if (found == module.projections.end()) module.projections.push_back(std::move(projection));
                return Type{Type::Projection, id};
            }
            return {};
        }
        if (owner.kind != Type::Record) return {};
        const auto& record = module.aggregates[owner.id];
        const auto& definition = sources[record.syntax.file];
        auto found = std::ranges::find_if(record.syntax.associated, [&](const auto& entry) { return entry.name.text == member; });
        if (found == record.syntax.associated.end() || (!found->isPublic && source.moduleId != definition.moduleId)) return {};
        auto key = std::pair(owner, member);
        if (std::ranges::find(resolvingAssociated, key) != resolvingAssociated.end())
            throw at(source, site, "LYG0007", "关联类型形成循环：" + typeName(module, owner) + '.' + member);
        if (resolvingAssociated.size() >= 128) throw at(source, site, "LYG0004", "关联类型展开过深：" + member);
        resolvingAssociated.push_back(std::move(key));
        struct Pop { decltype(resolvingAssociated)& stack; ~Pop() { stack.pop_back(); } } restore{resolvingAssociated};
        return resolveWith(typeForm(found->type.text), definition, found->type.span, record.syntax.typeParameters,
            [&](std::size_t slot) { return record.arguments[slot]; });
    }

    Type projectionType(Type owner, const std::string& member, const Source& source, Span site) {
        if (auto type = associatedType(owner, member, source, site)) return *type;
        throw at(source, site, "LYG0007", typeName(module, owner) + " 没有可见关联类型 " + member);
    }

    Type recordType(std::size_t declaration, std::vector<Type> arguments, const Source& source, Span span) {
        const auto& syntax = recordDeclarations[declaration];
        if (arguments.size() != syntax.typeParameters.size())
            throw at(source, span, "LYG0001", syntax.name.text + " 需要 " + std::to_string(syntax.typeParameters.size()) + " 个类型实参");
        if (auto found = recordInstances.find({declaration, arguments}); found != recordInstances.end()) return found->second;
        if (std::ranges::find(activeNominals, std::pair{false, declaration}) != activeNominals.end())
            throw at(source, span, "LYG0004", "递归类型在展开时改变了类型实参：" + syntax.name.text);
        if (expandingNominal >= 128 || recordInstances.size() >= 4096)
            throw at(source, span, "LYG0004", "记录类型实例展开过深或超出编译单元实例预算：" + syntax.name.text);
        Type type{Type::Record, module.aggregates.size()};
        bool symbolic = std::ranges::any_of(arguments, [&](Type argument) { return abstract(argument); });
        recordInstances.emplace(std::pair{declaration, arguments}, type);
        auto& record = module.aggregates.emplace_back(syntax, std::vector<Type>{}, declaration, std::move(arguments), symbolic);
        activeNominals.emplace_back(false, declaration);
        struct Pop { std::vector<std::pair<bool, std::size_t>>& stack; ~Pop() { stack.pop_back(); } } restore{activeNominals};
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
        if (std::ranges::find(activeNominals, std::pair{true, declaration}) != activeNominals.end())
            throw at(source, span, "LYG0004", "递归枚举在展开时改变了类型实参：" + syntax.name.text);
        if (expandingNominal >= 128 || enumInstances.size() >= 4096)
            throw at(source, span, "LYG0004", "枚举类型实例展开过深或超出编译单元实例预算：" + syntax.name.text);
        Type type{Type::Enum, module.enums.size()};
        bool symbolic = std::ranges::any_of(arguments, [&](Type argument) { return abstract(argument); });
        enumInstances.emplace(std::pair{declaration, arguments}, type);
        auto& instance = module.enums.emplace_back(syntax, std::vector<std::vector<Type>>{}, declaration, std::move(arguments), symbolic);
        activeNominals.emplace_back(true, declaration);
        struct Pop { std::vector<std::pair<bool, std::size_t>>& stack; ~Pop() { stack.pop_back(); } } restore{activeNominals};
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
            if (form.name == "$never") return Type::Never;
            if (form.name.starts_with("$dyn:")) {
                auto trait = traitNames.find(form.name.substr(5));
                if (trait == traitNames.end()) throw at(source, span, "LYG0005", "找不到动态接口 trait：" + form.name.substr(5));
                if (!module.traits[trait->second].associated.empty())
                    throw at(source, span, "LYG0007", "含关联类型的 trait 只能用作静态约束：" + form.name.substr(5));
                return Type{Type::Dyn, trait->second};
            }
            for (Type type : scalarTypes) if (form.name == typeName(module, type)) return type;
            if (auto exception = exceptionTypes.find(form.name); exception != exceptionTypes.end()) return Type{Type::Exception, exception->second + firstDeclaredException};
            if (auto alias = aliases.find(form.name); alias != aliases.end()) {
                if (std::ranges::find(resolvingAliases, form.name) != resolvingAliases.end())
                    throw at(source, span, "LYT0003", "透明类型别名形成循环：" + form.name);
                const auto& declaration = aliasDeclarations[alias->second];
                resolvingAliases.push_back(form.name);
                struct Pop { std::vector<std::string>& stack; ~Pop() { stack.pop_back(); } } restore{resolvingAliases};
                return resolveWith(typeForm(declaration.target.text), sources[declaration.file], declaration.target.span, {}, NoTypeLookup{});
            }
        }
        std::vector<Type> arguments;
        for (const auto& part : form.arguments) {
            auto value = resolveWith(part, source, span, parameters, lookup);
            if (!value) return std::nullopt;
            arguments.push_back(*value);
        }
        if (form.name.starts_with("$assoc:")) return projectionType(arguments.front(), form.name.substr(7), source, span);
        if (form.name == "$fn") {
            auto result = arguments.back();
            arguments.pop_back();
            return closure(std::move(arguments), result);
        }
        if (arguments.size() == 1 && form.name == "Task") return task(arguments[0]);
        if (arguments.size() == 1 && form.name == "TaskLocal") return taskLocal(arguments[0]);
        if (arguments.size() == 1 && form.name == "Channel") return channel(arguments[0]);
        if (arguments.size() == 1 && form.name == "List") return list(arguments[0]);
        if (form.name == "Range") {
            if (arguments.size() == 1 && arguments.front() == Type::Int) return Type::IntRange;
            throw at(source, span, "LYT0003", "区间类型目前只支持 Range<Int>");
        }
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
        CheckedFunction function{std::move(syntax), result, std::move(locals), std::move(expressions)};
        for (const auto& annotation : function.syntax.throwsTypes) {
            const auto& source = sources[function.syntax.file];
            auto type = resolve(function.syntax, annotation, arguments);
            if (type.kind != Type::Exception) throw at(source, annotation.span, "LYE0003", "throws 需要 Exception 或其具名子类型");
            if (std::ranges::contains(function.throwsTypes, type)) throw at(source, annotation.span, "LYE0003", "throws 重复标注了同一个异常类型");
            function.throwsTypes.push_back(type);
        }
        return function;
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

Diagnostic uncoveredPattern(const Source& source, Span site, PatternUse use, const std::string& missing) {
    return at(source, site, "LYT0012", (use == PatternUse::Binding ? "val 解构必须匹配所有可能的值；例如不匹配 " : "match 未穷尽，缺少分支：") + missing);
}

struct Checker {
    CheckedFunction& fn;
    const Source& source;
    CheckedModule& module;
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

    void requireValueOperation(Type type, ValueOperation operation, Span site) {
        auto support = symbols.valueSupport(type, operation);
        if (support.missing) throw at(source, site, "LYT0006", typeName(module, type) + "：" + *support.missing);
        if (materializing()) {
            symbols.bindValueOperations(operation, support, source, site);
            for (auto [owner, candidate] : support.overrides) {
                const auto& method = symbols.declaration(symbols.callables[candidate]);
                fn.calls.push_back({method.file, method.name.span});
            }
        }
    }

    std::optional<ExprId> currentWorker() const { return workers.empty() ? std::nullopt : std::optional(workers.back().id); }
    bool inAsync() const { return workers.empty() ? fn.syntax.isAsync : std::holds_alternative<AsyncBlock>(fn.syntax.expressions[workers.back().id].node); }
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
        if (!name.text.starts_with('$')) {
            fn.locals[slot].declaration = name.span;
            fn.locals[slot].name = name.text;
        }
    }

    void refer(const Identifier& name, std::size_t file, Span span) {
        if (!name.text.starts_with('$')) fn.references.push_back({{fn.syntax.file, name.span}, {file, span}, name.renameTarget});
    }

    void referLocal(const Identifier& name, std::size_t slot) {
        if (auto declaration = fn.locals[slot].declaration) fn.references.push_back({{fn.syntax.file, name.span}, {fn.syntax.file, *declaration}, {}});
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

    ErrorDisplay displayError(Type type, Span site, bool requireMessage, bool useMethod = true) {
        if (type == Type::String || type == Type::Error) return {};
        auto hasMessage = [](const RequiredMethod& shape) {
            return shape.parameters.empty() && !shape.asynchronous && shape.result == Type::String;
        };
        if (useMethod) {
            if (auto method = symbols.boundMember(type, "message", source, site); method && hasMessage(method->shape)) return {};
            if (auto selected = symbols.shapedMethod(type, "message", source, hasMessage)) {
                if (!materializing()) return {};
                const auto& item = symbols.callables[selected->first];
                return {item.generic ? symbols.instantiate(selected->first, module.aggregates[type.id].arguments, source, site) : item.target, {}};
            }
        }
        auto name = typeName(module, type);
        if (requireMessage) throw at(source, site, "LYE0003", name + " 不能提升为 Error：需要可见的同步实例方法 fn message(): String");
        return {{}, std::move(name)};
    }

    Type fit(ExprId id, Type type, std::optional<Type> expected) {
        auto& info = fn.expressions[id];
        if (expected && type != *expected && type != Type::Never) {
            auto site = fn.syntax.expressions[id].span;
            auto exceptionUpcast = type.kind == Type::Exception && *expected == Type::Exception;
            if (*expected == Type::Error) info.errorPromotion = displayError(type, site, true);
            else if (*expected != Type::Any && !exceptionUpcast && !losslessNumber(type, *expected) && (expected->kind != Type::Option || module.sumTypes[expected->id].success != type)) {
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
                    auto expected = symbols.requiredShape(actual, trait, method, source, operation.site);
                    if (auto builtin = symbols.builtinMember(actual, method.name.text)) {
                        if (builtin->shape != expected)
                            throw at(source, operation.site, "LYT0021", "内置成员签名与动态接口要求不匹配：" + method.name.text);
                        implementations.push_back(symbols.builtinImplementation(actual, method.name.text, *builtin, fn.syntax.file, operation.site));
                        continue;
                    }
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
            if (auto method = symbols.boundMember(type, "toString", source, fn.syntax.expressions[id].span); method && method->shape.parameters.empty() && !method->shape.asynchronous && method->shape.result == Type::String) return Type::String;
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
        auto result = visit(attempt.body, discard ? std::nullopt : expected, discard);
        auto exits = result == Type::Never ? std::optional<Present>{} : std::optional(present);
        std::set<Type> covered;
        for (const auto& arm : attempt.arms) {
            present = entry;
            scopes.emplace_back();
            auto type = arm.binding ? symbols.resolve(fn.syntax, arm.binding->type, variables) : Type{Type::Exception};
            if (type.kind != Type::Exception) throw at(source, arm.binding->type.span, "LYE0002", "catch 需要 Exception 或其具名子类型");
            if (covered.contains(Type::Exception) || covered.contains(type)) throw at(source, arm.site, "LYE0002", "此 catch 已被前面的无过滤捕获覆盖");
            if (!arm.guard) covered.insert(type);
            std::optional<std::size_t> local;
            if (arm.binding) {
                if (type.id >= firstDeclaredException) {
                    const auto& declaration = module.exceptions[type.id - firstDeclaredException];
                    refer(arm.binding->type, declaration.file, declaration.name.span);
                }
                local = fn.locals.size();
                fn.locals.push_back({type, false, currentWorker()});
                declare(arm.binding->name, *local);
            }
            fn.expressions[id].catches.push_back({type, local});
            catchOwners.push_back(currentWorker());
            bool reaches = !arm.guard || visit(*arm.guard, Type::Bool) != Type::Never;
            if (arm.guard && reaches) {
                auto facts = definitelyPresent(*arm.guard, true);
                present.insert(facts.begin(), facts.end());
            }
            auto recovered = visit(arm.body, discard ? std::nullopt : result != Type::Never ? std::optional(result) : expected, discard);
            catchOwners.pop_back();
            scopes.pop_back();
            if (reaches && recovered != Type::Never) {
                if (result == Type::Never) result = recovered;
                exits = exits ? bothPaths(*exits, present) : present;
            }
        }
        present = exits.value_or(std::move(entry));
        return result != Type::Never && discard ? Type{Type::Unit} : result;
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
            const auto& node = fn.syntax.expressions[workers.back().id].node;
            if (std::holds_alternative<Lambda>(node)) throw at(source, propagation.site, "LYT0013", "请给 lambda 标注 Option 或 Result 的返回类型以使用 ?");
            auto group = std::get_if<AsyncBlock>(&node);
            throw at(source, propagation.site, "LYT0013", group && group->kind == AsyncBlock::Group ? "请给 taskGroup 块明确的 Option 或 Result 目标类型以使用 ?" : "请给任务块明确的 Task<Option<T>> 或 Task<Result<T,E>> 目标类型以使用 ?");
        }
        if (expected && (target->kind == Type::Option || target->kind == Type::Result)) hint = symbols.sum(target->kind, *expected, module.sumTypes[target->id].failure);
        auto operand = inspectOption(propagation.operand, hint);
        if (operand == Type::Never) return operand;
        if (operand.kind != Type::Option && operand.kind != Type::Result) throw at(source, propagation.site, "LYT0013", "? 需要 Option 或 Result，实际是 " + typeName(module, operand));
        if (operand.kind == Type::Result && target->kind == Type::Result) {
            const auto& from = module.sumTypes[operand.id];
            auto to = module.sumTypes[target->id].failure;
            if (from.failure == to) return from.success;
            auto& info = fn.expressions[id];
            auto input = from.failure, success = from.success;
            if (to == Type::Error) {
                info.errorDisplay = displayError(input, propagation.site, true);
                return success;
            }
            info.errorConversion = directConversion(symbols.conversions, module, symbols.sources, input, to, source, propagation.site, !probing);
            if (info.errorConversion) {
                info.errorDisplay = displayError(input, propagation.site, false, false);
                return success;
            }
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
            if (found == symbols.records.end())
                throw at(source, name.span, "LYT0011", "此模式需要可见的 struct、class 或匹配类型的枚举分支：" + name.text);
            if (subject.kind != Type::Record || module.aggregates[subject.id].declaration != found->second)
                throw at(source, name.span, "LYT0011", name.text + " 不能匹配 " + typeName(module, subject));
            const auto& owner = module.aggregates[subject.id];
            refer(name, owner.syntax.file, owner.syntax.name.span);
            auto constructor = owner.syntax.reference ? PatternConstructor(PatternClass{subject.id}) : PatternConstructor(PatternProduct{});
            CheckedPattern result{std::move(constructor), std::vector<CheckedPattern>(owner.fields.size())};
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
        if (!named.escaped) {
            auto candidates = symbols.visibleFunctions(source, name);
            for (auto candidate : candidates) {
                const auto& declaration = symbols.declaration(symbols.callables[candidate]);
                if (declaration.constant != ConstKind::Value) continue;
                auto target = symbols.callables[candidate].target;
                auto type = module.functions[target].result;
                if (subject != type) throw mismatch(module, source, pattern.span, subject, type);
                requireValueOperation(type, ValueOperation::Equal, pattern.span);
                refer(name, declaration.file, declaration.name.span);
                return {PatternConstant{target, type}};
            }
        }
        auto slot = fn.locals.size();
        fn.locals.push_back({subject, false, currentWorker()});
        declare(name, slot);
        return {PatternSlot{slot}};
    }

    void remapPatternSlots(CheckedPattern& pattern, const std::unordered_map<std::size_t, std::size_t>& slots) {
        if (auto* slot = std::get_if<PatternSlot>(&pattern.node)) slot->local = slots.at(slot->local);
        if (auto* rest = std::get_if<PatternListRest>(&pattern.node)) rest->local = slots.at(rest->local);
        for (auto& part : pattern.parts) remapPatternSlots(part, slots);
    }

    CheckedPattern alternativesPattern(const Pattern& pattern, Type subject) {
        std::vector<CheckedPattern> checked;
        std::vector<Names> bindings;
        for (const auto& alternative : pattern.parts) {
            scopes.emplace_back();
            checked.push_back(checkPattern(alternative, subject));
            bindings.push_back(std::move(scopes.back()));
            scopes.pop_back();
        }
        const auto& canonical = bindings.front();
        for (std::size_t index = 1; index < bindings.size(); ++index) {
            if (bindings[index].size() != canonical.size())
                throw at(source, pattern.parts[index].span, "LYT0011", "OR 模式的每个分支必须绑定相同名称和类型");
            for (const auto& [name, slot] : canonical) {
                auto found = bindings[index].find(name);
                if (found == bindings[index].end() || fn.locals[found->second].type != fn.locals[slot].type)
                    throw at(source, pattern.parts[index].span, "LYT0011", "OR 模式的每个分支必须绑定相同名称和类型");
            }
        }
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            std::unordered_map<std::size_t, std::size_t> slots;
            for (const auto& [name, slot] : bindings[index]) {
                auto canonicalSlot = canonical.at(name);
                slots.emplace(slot, canonicalSlot);
                if (index) {
                    auto sourceBinding = fn.locals[slot].declaration;
                    auto canonicalBinding = fn.locals[canonicalSlot].declaration;
                    if (sourceBinding && canonicalBinding)
                        fn.references.push_back({{fn.syntax.file, *sourceBinding}, {fn.syntax.file, *canonicalBinding}, {}});
                }
            }
            remapPatternSlots(checked[index], slots);
        }
        for (const auto& [name, slot] : canonical) {
            if (!scopes.back().emplace(name, slot).second) {
                auto declaration = fn.locals[slot].declaration.value_or(pattern.span);
                throw at(source, declaration, "LYN0002", "名称已在此模式作用域声明");
            }
        }
        return {CheckedAlternatives{}, std::move(checked)};
    }

    CheckedPattern checkPattern(const Pattern& pattern, Type subject) {
        const auto& node = pattern.node;
        if (std::holds_alternative<PatternAlternatives>(node)) return alternativesPattern(pattern, subject);
        if (const auto* list = std::get_if<PatternList>(&node)) {
            if (subject.kind != Type::List) throw at(source, pattern.span, "LYT0011", "列表模式不能匹配 " + typeName(module, subject));
            auto element = module.listElements[subject.id];
            CheckedPattern tail;
            if (list->rest) {
                auto slot = fn.locals.size();
                fn.locals.push_back({subject, false, currentWorker()});
                declare(*list->rest, slot);
                tail.node = PatternListRest{slot};
            } else if (!list->openEnd) tail.node = PatternConstructor(ListTag{false, element, subject});
            for (std::size_t index = pattern.parts.size(); index-- > 0;) {
                auto head = checkPattern(pattern.parts[index], element);
                tail = {PatternConstructor(ListTag{true, element, subject}), {std::move(head), std::move(tail)}};
            }
            return tail;
        }
        if (auto* named = std::get_if<PatternName>(&node)) return namedPattern(pattern, subject, *named);
        if (auto* tag = std::get_if<SumTag>(&node)) return sumPattern(pattern, subject, *tag);
        if (std::holds_alternative<std::monostate>(node)) return {};
        if (auto* range = std::get_if<PatternIntegerRange>(&node)) {
            if (!integerType(subject)) throw at(source, pattern.span, "LYT0011", "整数区间模式不能匹配 " + typeName(module, subject));
            checkIntegerLiteral(pattern.span, subject, range->lower);
            checkIntegerLiteral(pattern.span, subject, range->upper);
            return {CheckedIntegerRange{integerBits(range->lower), integerBits(range->upper), range->inclusive, subject.kind == Type::Int}};
        }
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

    void checkExhaustiveness(Type subject, Span site, std::span<const CheckedPattern* const> patterns, PatternUse use) {
        auto hasConstant = [&](auto&& self, const CheckedPattern& pattern) -> bool {
            return std::holds_alternative<PatternConstant>(pattern.node) ||
                std::ranges::any_of(pattern.parts, [&](const auto& part) { return self(self, part); });
        };
        if (std::ranges::any_of(patterns, [&](const auto* pattern) { return hasConstant(hasConstant, *pattern); })) {
            std::vector<CheckedPattern> deferred;
            for (const auto* pattern : patterns) deferred.push_back(*pattern);
            module.deferredPatternChecks.push_back({fn.syntax.file, site, subject, std::move(deferred), use});
        } else if (auto missing = missingPattern(module, subject, patterns, use)) throw uncoveredPattern(source, site, use, *missing);
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
        checkExhaustiveness(subject, fn.syntax.expressions[id].span, patterns, PatternUse::Match);
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
        const auto& syntax = symbols.declaration(item);
        refer(name, syntax.file, syntax.name.span);
        if (syntax.constant == ConstKind::Value) {
            fn.expressions[id].constantRef = item.target;
            return module.functions[item.target].result;
        }
        auto [type, types] = functionValue(index, name, expected);
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

    Type infer(ExprId id, const PatternBinding& binding, auto, bool) {
        auto annotation = binding.annotation ? std::optional(symbols.resolve(fn.syntax, *binding.annotation, variables)) : std::nullopt;
        auto value = visit(binding.value, annotation);
        if (value == Type::Never) return value;
        auto& checked = fn.expressions[id].patterns;
        checked.push_back(checkPattern(binding.pattern, value));
        const CheckedPattern* pattern = &checked.front();
        checkExhaustiveness(value, binding.pattern.span, {&pattern, 1}, PatternUse::Binding);
        return Type::Unit;
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
        if (owner == Type::IntRange) {
            for (std::size_t slot = 0; slot < std::size(rangeFields); ++slot) if (member.name.text == rangeFieldNames[slot]) {
                fn.expressions[id].target = slot;
                return rangeFields[slot];
            }
        }
        if (owner == Type::String && (member.name.text == "size" || member.name.text == "byteSize")) return Type::Int;
        if (owner == Type::Bytes && member.name.text == "size") return Type::Int;
        if (owner == Type::Char && member.name.text == "codePoint") return number(Type::UInt, 32);
        if (owner.kind == Type::Exception) {
            auto slot = [&](ExceptionMember field, Type type) { fn.expressions[id].target = static_cast<std::size_t>(field); return type; };
            if (member.name.text == "message") return slot(ExceptionMember::Message, Type::String);
            if (member.name.text == "taskFailures") return slot(ExceptionMember::TaskFailures, symbols.list(Type::Exception));
            if (member.name.text == "cleanupFailures") return slot(ExceptionMember::CleanupFailures, symbols.list(Type::Exception));
        }
        if (owner == Type::Error) {
            auto slot = [&](ErrorMember field, Type type) { fn.expressions[id].target = static_cast<std::size_t>(field); return type; };
            if (member.name.text == "message") return slot(ErrorMember::Message, Type::String);
            if (member.name.text == "original") return slot(ErrorMember::Original, Type::Any);
            if (member.name.text == "cause") return slot(ErrorMember::Cause, symbols.sum(Type::Option, Type::Error));
            if (member.name.text == "suppressed") return slot(ErrorMember::Suppressed, symbols.list(Type::Error));
        }
        if (owner.kind == Type::Record) {
            auto declaration = module.aggregates[owner.id].declaration;
            if (auto field = symbols.fields[declaration].find(member.name.text); field != symbols.fields[declaration].end()) {
                fn.expressions[id].target = field->second;
                const auto& record = module.aggregates[owner.id];
                visible(record.syntax.file, record.syntax.fields[field->second].isPublic, member.name);
                refer(member.name, record.syntax.file, record.syntax.fields[field->second].name.span);
                return record.fields[field->second];
            }
        }
        std::vector<std::pair<std::size_t, std::vector<Type>>> properties;
        for (auto extension : symbols.visibleExtensions[fn.syntax.file]) {
            auto property = symbols.extensionProperties[extension].find(member.name.text);
            if (property == symbols.extensionProperties[extension].end()) continue;
            auto types = symbols.extensionTypes(extension, owner);
            if (types) properties.emplace_back(extension, std::move(*types));
        }
        if (properties.size() > 1) throw at(source, member.name.span, "LYT0019", "扩展属性存在歧义：" + member.name.text);
        if (!properties.empty()) {
            auto [extension, types] = std::move(properties.front());
            const auto& property = symbols.extensionDeclarations[extension].properties[symbols.extensionProperties[extension].at(member.name.text)];
            auto callable = symbols.functionCallables[property.getter];
            Call getter{member.name, {}, member.receiver};
            auto result = declaredCall(callable, id, getter, expected, types);
            fn.expressions[id].callee = Callee::ExtensionProperty;
            return result;
        }
        throw at(source, member.name.span, "LYN0001", typeName(module, owner) + " 没有成员：" + member.name.text);
    }

    std::vector<std::optional<Type>> argumentHints(ExprId id, const Call& call, const Function& target,
            const std::vector<bool>& checked, std::span<const Type> known, std::optional<Type> expected) const {
        std::vector<std::optional<Type>> inferred(target.typeParameters.size());
        std::copy(known.begin(), known.end(), inferred.begin());
        auto learn = [&](auto&& self, const TypeForm& key, Type type, bool widen) -> void {
            if (key.name.starts_with("$assoc:")) return;
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
        if (syntax.constant == ConstKind::Value) throw at(source, call.name.span, "LYCT0001", "编译期常量不是函数：" + call.name.text);
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
        if (info.sourceType && (info.type == Type::Exception || info.type == Type::Error)) return {MatchKind::Upcast};
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
        if (call.receiver && !call.typeOwner) {
            const auto* qualifier = std::get_if<Identifier>(&fn.syntax.expressions[*call.receiver].node);
            if (qualifier && linkedName(*qualifier) == "Task" && !lookupLocal(*qualifier) && !implicitMember(*qualifier) &&
                (call.name.text == "cancelled" || call.name.text == "checkCancellation")) {
                noTypeArgs();
                if (!call.arguments.empty()) throw at(source, call.name.span, "LYT0002", "Task." + call.name.text + " 不接收实参");
                if (!inAsync()) throw at(source, call.name.span, "LYT0008", "Task." + call.name.text + " 只能在异步执行体中调用");
                info.callee = call.name.text == "cancelled" ? Callee::TaskCancelled : Callee::TaskCheckCancellation;
                return call.name.text == "cancelled" ? Type{Type::Bool} : Type{Type::Unit};
            }
        }
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
            if (owner == Type::Any && (call.name.text == "typeName" || call.name.text == "fieldCount" ||
                call.name.text == "fieldNames" || call.name.text == "field")) {
                noTypeArgs();
                info.callee = call.name.text == "typeName" ? Callee::ReflectTypeName :
                    call.name.text == "fieldCount" ? Callee::ReflectFieldCount :
                    call.name.text == "fieldNames" ? Callee::ReflectFieldName : Callee::ReflectField;
                if (call.name.text == "field") {
                    if (arguments(id, call, 1, [](auto) { return "name"; }, [](auto) { return Type{Type::String}; })) return Type::Never;
                    return symbols.sum(Type::Option, Type::Any);
                }
                if (!call.arguments.empty()) throw at(source, call.name.span, "LYT0002", "Any." + call.name.text + " 不接收实参");
                if (call.name.text == "typeName") return Type::String;
                if (call.name.text == "fieldCount") return Type::Int;
                return symbols.list(Type::String);
            }
            bool privateMember = false, staticMember = false;
            if (owner.kind == Type::Result && call.name.text == "failureInfo") {
                noTypeArgs();
                if (!call.arguments.empty()) throw at(source, call.name.span, "LYT0002", "failureInfo 不接收实参");
                info.callee = Callee::FailureInfo;
                info.errorDisplay = displayError(module.sumTypes[owner.id].failure, call.name.span, false);
                return symbols.sum(Type::Option, Type::Error);
            }
            if (owner == Type::Error && call.name.text == "withSuppressed") {
                noTypeArgs();
                info.callee = Callee::ErrorSuppress;
                return arguments(id, call, 1, [](auto) { return "error"; }, [](auto) { return Type{Type::Error}; }) ? Type::Never : Type::Error;
            }
            if (auto builtin = symbols.builtinMember(owner, call.name.text)) {
                noTypeArgs();
                info.callee = builtin->callee;
                const auto& shape = builtin->shape;
                return arguments(id, call, shape.parameters.size(), [&](auto slot) -> std::string_view { return shape.labels[slot]; },
                    [&](auto slot) { return shape.parameters[slot]; }) ? Type{Type::Never} : shape.result;
            }
            auto valueMember = [&](ValueOperation operation) {
                bool equality = operation == ValueOperation::Equal;
                noTypeArgs();
                auto exits = arguments(id, call, equality ? 1u : 0u, [](auto) { return "other"; }, [&](auto) { return owner; });
                requireValueOperation(owner, operation, call.name.span);
                info.callee = equality ? Callee::ValueEqual : Callee::ValueHash;
                if (owner.kind == Type::Variable) info.boundTrait = equality ? "Eq" : "Hash";
                return exits ? Type{Type::Never} : equality ? Type::Bool : Type::Int;
            };
            if (owner.kind == Type::Variable) {
                auto capability = call.name.text == "equals" ? "Eq" : call.name.text == "hashCode" ? "Hash" : "";
                auto constraints = symbols.contracts.constraints(owner);
                if (std::ranges::find(constraints, capability) != constraints.end()) return valueMember(valueConstraint(capability).value());
                if (auto method = symbols.boundMember(owner, call.name.text, source, call.name.span)) {
                    noTypeArgs();
                    info.boundTrait = method->trait->name.text;
                    refer(call.name, method->trait->file, method->method->name.span);
                    const auto& shape = method->shape;
                    bool exits = arguments(id, call, shape.parameters.size(), [&](auto slot) -> std::string_view { return shape.labels[slot]; }, [&](auto slot) { return shape.parameters[slot]; });
                    return exits ? Type{Type::Never} : shape.asynchronous ? symbols.task(shape.result) : shape.result;
                }
            }
            if (owner.kind == Type::Dyn) {
                const auto& trait = module.traits[owner.id];
                auto declaration = std::ranges::find(trait.methods, call.name.text, [](const TraitMethod& candidate) { return candidate.name.text; });
                if (declaration != trait.methods.end()) {
                    noTypeArgs();
                    auto method = symbols.requiredShape(owner, trait, *declaration, source, call.name.span);
                    refer(call.name, trait.file, declaration->name.span);
                    info.callee = Callee::Dynamic;
                    info.target = declaration - trait.methods.begin();
                    info.interfaceAsync = method.asynchronous;
                    bool exits = arguments(id, call, method.parameters.size(), [&](auto slot) -> std::string_view { return method.labels[slot]; }, [&](auto slot) { return method.parameters[slot]; });
                    return exits ? Type{Type::Never} : method.asynchronous ? symbols.task(method.result) : method.result;
                }
            }
            if (call.boundTrait) if (auto operation = valueConstraint(*call.boundTrait)) return valueMember(*operation);
            if (owner.kind == Type::Record) {
                const auto& decl = module.aggregates[owner.id];
                ownerTypes = decl.arguments;
                if (call.boundTrait) {
                    const auto& trait = symbols.contracts.declaration(*call.boundTrait);
                    const auto& method = *std::ranges::find_if(trait.methods, [&](const auto& entry) { return entry.name.text == call.name.text; });
                    auto required = symbols.requiredShape(owner, trait, method, source, call.name.span);
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
            if (call.name.text == "equals" || call.name.text == "hashCode") {
                if (!privateMember && !staticMember) return valueMember(call.name.text == "equals" ? ValueOperation::Equal : ValueOperation::Hash);
            }
            if (owner.kind == Type::Task && call.name.text == "cancel") {
                if (!inAsync()) throw at(source, call.name.span, "LYT0008", "Task.cancel() 只能在异步执行体中调用，不能在同步 lambda、blocking 或 defer 中调用");
                noTypeArgs();
                if (!call.arguments.empty()) throw at(source, call.name.span, "LYT0002", "Task.cancel() 不接收实参");
                info.callee = Callee::TaskCancel;
                return Type::Unit;
            }
            if (owner.kind == Type::TaskLocal && call.name.text == "get") {
                noTypeArgs();
                if (!call.arguments.empty()) throw at(source, call.name.span, "LYT0002", "TaskLocal.get 不接收实参");
                info.callee = Callee::LocalGet;
                return symbols.sum(Type::Option, module.taskLocalValues[owner.id]);
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
        } else if (key == "Channel") {
            std::optional<Type> type;
            if (call.typeArguments.size() == 1) type = symbols.channel(symbols.resolve(fn.syntax, call.typeArguments.front(), variables));
            else if (call.typeArguments.empty()) {
                if (expected && expected->kind == Type::Option) expected = module.sumTypes[expected->id].success;
                if (expected && expected->kind == Type::Channel) type = expected;
            }
            if (!type) throw at(source, call.name.span, "LYG0001", "Channel 需要一个类型实参或明确的 Channel<T> 目标类型");
            info.callee = Callee::ChannelCreate;
            return arguments(id, call, 1, [](auto) { return "capacity"; }, [](auto) { return Type{Type::Int}; }) ? Type{Type::Never} : *type;
        } else if (key == "sleepMs") {
            noTypeArgs();
            info.callee = Callee::Sleep;
            return arguments(id, call, 1, [](auto) { return "milliseconds"; }, [](auto) { return Type{Type::Int}; }) ? Type{Type::Never} : symbols.task(Type::Unit);
        } else if (key == "TaskLocal") {
            if (!call.arguments.empty()) throw at(source, call.name.span, "LYT0002", "TaskLocal 不接收值实参");
            info.callee = Callee::LocalKey;
            if (call.typeArguments.size() == 1) return symbols.taskLocal(symbols.resolve(fn.syntax, call.typeArguments.front(), variables));
            if (call.typeArguments.empty()) {
                if (expected && expected->kind == Type::Option) expected = module.sumTypes[expected->id].success;
                if (expected && expected->kind == Type::TaskLocal) return *expected;
            }
            throw at(source, call.name.span, "LYG0001", "TaskLocal 需要一个类型实参或明确的 TaskLocal<T> 目标类型");
        } else if (key == "withLocal") {
            noTypeArgs();
            if (call.arguments.size() != 3 || std::ranges::any_of(call.arguments, [](const Argument& item) { return item.label || item.spread || item.trailing; }))
                throw at(source, call.name.span, "LYT0002", "withLocal 写作 withLocal(key, value, task)");
            auto key = visit(call.arguments[0].value);
            if (key == Type::Never) return key;
            if (key.kind != Type::TaskLocal) throw at(source, fn.syntax.expressions[call.arguments[0].value].span, "LYT0008", "withLocal 的第一个实参需要 TaskLocal<T>");
            auto value = visit(call.arguments[1].value, module.taskLocalValues[key.id]);
            auto task = visit(call.arguments[2].value, expected && expected->kind == Type::Task ? expected : std::nullopt);
            if (task != Type::Never && task.kind != Type::Task) throw at(source, fn.syntax.expressions[call.arguments[2].value].span, "LYT0008", "withLocal 的第三个实参需要冷 Task 或已启动的 Task");
            info.callee = Callee::WithLocal;
            info.argumentSlots = {0, 1, 2};
            return value == Type::Never ? value : task;
        } else if (key == "Error") {
            noTypeArgs();
            if (call.arguments.empty() || call.arguments.size() > 2) throw at(source, call.name.span, "LYT0002", "Error 需要 value 及可选的 cause 实参");
            info.callee = Callee::BusinessError;
            auto cause = symbols.sum(Type::Option, Type::Error);
            if (arguments(id, call, call.arguments.size(), [](auto slot) { return slot ? "cause" : "value"; }, [&](auto slot) -> std::optional<Type> { return slot ? std::optional(cause) : std::nullopt; })) return Type::Never;
            auto value = call.arguments[std::ranges::find(info.argumentSlots, 0) - info.argumentSlots.begin()].value;
            auto type = fn.expressions[value].type;
            if (type == Type::Error) throw at(source, fn.syntax.expressions[value].span, "LYE0003", "已有 Error 值无需再次包装");
            info.errorDisplay = displayError(type, fn.syntax.expressions[value].span, true);
            return Type::Error;
        } else if (key == "Exception" || key == "ChannelClosed") {
            noTypeArgs();
            info.callee = Callee::Exception;
            return arguments(id, call, 1, [](auto) { return "message"; }, [](auto) { return Type{Type::String}; }) ? Type{Type::Never} : key == "ChannelClosed" ? channelClosed : Type::Exception;
        } else if (auto named = symbols.exceptionTypes.find(key); named != symbols.exceptionTypes.end()) {
            noTypeArgs();
            const auto& type = module.exceptions[named->second];
            refer(call.name, type.file, type.name.span);
            info.callee = Callee::Exception;
            return arguments(id, call, 1, [](auto) { return "message"; }, [](auto) { return Type{Type::String}; }) ? Type{Type::Never} : Type{Type::Exception, named->second + firstDeclaredException};
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
        if (!inAsync()) throw at(source, op.site, "LYT0008", "await 和 spawn 只能在异步执行体中使用，不能用于同步 lambda、blocking 或 defer");
        auto operand = inspect(op.operand, op.spawn || !expected ? expected : std::optional(symbols.task(*expected)));
        if (operand.kind == Type::Never) return operand;
        if (operand.kind != Type::Task) throw at(source, fn.syntax.expressions[op.operand].span, "LYT0008", "此处需要 Task，实际是 " + typeName(module, operand));
        return op.spawn ? operand : module.taskResults[operand.id];
    }

    Type infer(ExprId id, const Select& selection, std::optional<Type> expected, bool discard) {
        if (!inAsync()) throw at(source, selection.site, "LYT0008", "await select 只能在异步执行体中使用");
        std::vector<Type> outcomes;
        for (const auto& arm : selection.arms) {
            auto task = inspect(arm.operation, {});
            auto callee = fn.expressions[arm.operation].callee;
            if (!std::holds_alternative<Call>(fn.syntax.expressions[arm.operation].node) ||
                (callee != Callee::ChannelSend && callee != Callee::ChannelReceive && callee != Callee::Sleep))
                throw at(source, fn.syntax.expressions[arm.operation].span, "LYT0008", "select 分支必须直接调用 Channel.send、Channel.receive 或 sleepMs");
            if (task == Type::Never) return task;
            outcomes.push_back(module.taskResults[task.id]);
        }
        auto entry = present;
        auto& slots = fn.expressions[id].selectSlots;
        std::optional<Type> result;
        for (std::size_t index = 0; index < selection.arms.size(); ++index) {
            const auto& arm = selection.arms[index];
            present = entry;
            scopes.emplace_back();
            slots.emplace_back();
            if (arm.binding) {
                auto slot = fn.locals.size();
                fn.locals.push_back({outcomes[index], false, currentWorker()});
                declare(*arm.binding, slot);
                slots.back() = slot;
            }
            auto value = visit(arm.value, discard ? std::nullopt : result ? result : expected, discard);
            if (value != Type::Never && !result) result = discard ? Type::Unit : value;
            scopes.pop_back();
        }
        present = std::move(entry);
        return result.value_or(Type::Never);
    }

    Type taskBlock(ExprId id, ExprId bodyId, std::optional<Type> hint) {
        workers.push_back({id, fn.locals.size(), hint});
        unsigned previous = std::exchange(loops, 0);
        auto entry = present;
        auto body = visit(bodyId, hint);
        auto result = workers.back().result.value_or(body);
        fit(bodyId, body, result);
        workers.pop_back();
        present = std::move(entry);
        loops = previous;
        return result;
    }

    Type infer(ExprId id, const Blocking& worker, std::optional<Type> expected, bool) {
        auto hint = expected && expected->kind == Type::Task ? std::optional(module.taskResults[expected->id]) : std::nullopt;
        return symbols.task(taskBlock(id, worker.body, hint));
    }

    Type infer(ExprId id, const AsyncBlock& block, std::optional<Type> expected, bool discard) {
        if (!inAsync()) throw at(source, block.site, "LYT0008", "taskGroup 和 parallel 只能在异步执行体中使用");
        if (block.kind == AsyncBlock::Group) return taskBlock(id, block.body, discard ? std::nullopt : expected);
        auto hint = expected && expected->kind == Type::Task ? std::optional(module.taskResults[expected->id]) : std::nullopt;
        return symbols.task(taskBlock(id, block.body, hint));
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
        if (std::holds_alternative<Identifier>(expr.node)) {
            const auto& info = fn.expressions[id];
            return !info.callableType && !info.constantRef && fn.locals[info.target].writable;
        }
        if (auto member = std::get_if<Member>(&expr.node)) {
            if (fn.expressions[id].callee == Callee::ExtensionProperty) return false;
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

    Type infer(ExprId, const RangeValue& range, auto, bool) {
        auto start = visit(range.start, Type::Int);
        auto end = visit(range.end, Type::Int);
        return start == Type::Never || end == Type::Never ? Type{Type::Never} : Type{Type::IntRange};
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
        if (equality && left.kind == Type::Exception && right.kind == Type::Exception) {
            fit(binary.left, left, Type::Exception);
            fit(binary.right, right, Type::Exception);
            return Type::Bool;
        }
        right = fit(binary.right, right, left);
        if (equality) requireValueOperation(left, ValueOperation::Equal, binary.site);
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
            if (auto method = symbols.boundMember(owner, name, source, site); method && method->shape.parameters.empty() && !method->shape.asynchronous)
                return {method->shape.result, {}};
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
        auto input = visit(loop.input);
        auto element = input;
        bool exits = input == Type::Never;
        if (input == Type::IntRange) {
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
            if (valueConstraint(bound.trait.text)) continue;
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
        if (name == "Never" || name == "TaskLocal" || name == "Range" || name == "Eq" || name == "Hash" || name == "Channel") throw at(sources[decl.file], decl.name.span, "LYN0002", "类型名与内置类型冲突：" + std::string(name));
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
    for (auto& decl : module.exceptions) {
        reserveType(decl, symbols.exceptionTypes, checked.exceptions.size());
        checked.exceptions.push_back(std::move(decl));
    }
    for (auto& decl : module.aliases) {
        if (symbols.records.contains(decl.name.text) || symbols.enums.contains(decl.name.text) || symbols.exceptionTypes.contains(decl.name.text))
            throw at(sources[decl.file], decl.name.span, "LYN0002", "类型名称已声明：" + decl.name.text);
        reserveType(decl, symbols.aliases, symbols.aliasDeclarations.size());
        symbols.aliasDeclarations.push_back(std::move(decl));
    }
    for (std::size_t index = 0; index < symbols.aliasDeclarations.size(); ++index) {
        const auto& alias = symbols.aliasDeclarations[index];
        const auto& source = sources[alias.file];
        auto target = symbols.resolve(source, alias.target);
        if (alias.isPublic) expose(target, source, alias.target.span);
    }
    symbols.fields.resize(symbols.recordDeclarations.size());
    symbols.methods.resize(symbols.recordDeclarations.size());
    symbols.staticMethods.resize(symbols.recordDeclarations.size());
    symbols.initializers.resize(symbols.recordDeclarations.size());
    symbols.variants.resize(symbols.enumDeclarations.size());
    symbols.extensionDeclarations = std::move(module.extensions);
    symbols.extensionMethods.resize(symbols.extensionDeclarations.size());
    symbols.extensionProperties.resize(symbols.extensionDeclarations.size());
    symbols.functionCallables.resize(module.functions.size());
    symbols.visibleExtensions.resize(sources.size());
    for (std::size_t index = 0; index < symbols.extensionDeclarations.size(); ++index) {
        const auto& group = symbols.extensionDeclarations[index];
        reserveType(group, symbols.extensions, index);
        for (std::size_t property = 0; property < group.properties.size(); ++property) {
            const auto& name = group.properties[property].name;
            if (!symbols.extensionProperties[index].emplace(name.text, property).second)
                throw at(sources[group.file], name.span, "LYN0002", "扩展属性重复：" + name.text);
        }
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
        if (symbols.records.contains(trait.name.text) || symbols.enums.contains(trait.name.text) || symbols.aliases.contains(trait.name.text) || symbols.exceptionTypes.contains(trait.name.text)) throw at(source, trait.name.span, "LYN0002", "类型名称已声明：" + trait.name.text);
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
        std::set<std::string> associated;
        for (const auto& member : syntax.associated)
            if (!associated.insert(member.name.text).second) throw at(source, member.name.span, "LYN0002", "关联类型重复：" + member.name.text);
        for (std::size_t slot = 0; slot < syntax.fields.size(); ++slot) {
            const auto& field = syntax.fields[slot];
            if (!symbols.fields[id].emplace(field.name.text, slot).second) throw at(source, field.name.span, "LYN0002", "字段重复：" + field.name.text);
        }
        auto type = symbols.recordType(id, typeVariables(syntax), source, syntax.name.span);
        for (const auto& member : syntax.associated) {
            auto resolved = symbols.projectionType(type, member.name.text, source, member.type.span);
            if (syntax.isPublic && member.isPublic) expose(resolved, source, member.type.span);
        }
        if (syntax.isPublic) {
            exposeBounds(syntax);
            for (std::size_t slot = 0; slot < syntax.fields.size(); ++slot) if (syntax.fields[slot].isPublic)
                expose(checked.aggregates[type.id].fields[slot], source, syntax.fields[slot].type.span);
        }
    }
    for (const auto& trait : module.traits) {
        const auto& source = sources[trait.file];
        Type owner{Type::Unit};
        if (!trait.associated.empty()) {
            owner = {Type::Variable, checked.variableNames.size()};
            checked.variableNames.push_back("Self");
            symbols.contracts.bind(owner, trait.name, source);
        }
        for (const auto& method : trait.methods) {
            auto shape = symbols.requiredShape(owner, trait, method, source, method.name.span);
            if (!trait.isPublic) continue;
            expose(shape.result, source, method.result.span);
            for (std::size_t slot = 0; slot < method.parameters.size(); ++slot)
                expose(shape.parameters[slot], source, method.parameters[slot].type.span);
        }
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
        for (std::size_t index = 0; index < function.throwsTypes.size(); ++index)
            expose(function.throwsTypes[index], source, syntax.throwsTypes[index].span);
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
    for (std::size_t functionIndex = 0; functionIndex < module.functions.size(); ++functionIndex) {
        auto& function = module.functions[functionIndex];
        const auto& source = sources[function.file];
        auto owner = function.owner ? std::optional(symbols.records.at(function.owner->text)) : std::nullopt;
        auto group = function.extensionGroup ? std::optional(symbols.extensions.at(function.extensionGroup->text)) : std::nullopt;
        if (function.initializer) {
            if (symbols.initializers[*owner]) throw at(source, function.name.span, "LYN0002", "class 只允许一个 init");
            symbols.initializers[*owner] = symbols.callables.size();
        }
        else if (group) {
            if (!function.extensionProperty) {
                if (symbols.extensionProperties[*group].contains(function.name.text))
                    throw at(source, function.name.span, "LYN0002", "扩展成员名称重复：" + function.name.text);
                addOverload(symbols.extensionMethods[*group][function.name.text], function, source);
            }
        }
        else if (owner) {
            auto& names = function.hasReceiver ? symbols.methods[*owner] : symbols.staticMethods[*owner];
            if (function.hasReceiver && symbols.fields[*owner].contains(function.name.text))
                throw at(source, function.name.span, "LYN0002", "函数名已声明：" + function.name.text);
            addOverload(names[function.name.text], function, source);
        } else {
            const auto& name = function.name.text;
            if (symbols.records.contains(name) || symbols.enums.contains(name) || symbols.aliases.contains(name) || symbols.exceptionTypes.contains(name) || name == "print" || name == "gcCollect")
                throw at(source, function.name.span, "LYN0002", "函数名已声明：" + name);
            auto& candidates = symbols.functions[name];
            if (!candidates.empty() && (name == "main" || name.ends_with("::main"))) throw at(source, function.name.span, "LYN0002", "main 不能重载");
            if (!candidates.empty() && (function.constant == ConstKind::Value || symbols.declaration(symbols.callables[candidates.front()]).constant == ConstKind::Value))
                throw at(source, function.name.span, "LYN0002", "常量名已声明：" + name);
            addOverload(candidates, function, source);
        }
        if (!function.typeParameters.empty()) {
            if (!owner && !group && requireEntry && function.name.text == "main") throw at(source, function.name.span, "LYG0001", "main 不能声明类型形参");
            auto variables = typeVariables(function);
            symbols.functionCallables[functionIndex] = symbols.callables.size();
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
                if (concrete.result != expected && concrete.result != symbols.sum(Type::Result, Type::Unit, Type::Error))
                    throw at(source, syntax.result.span, "LYT0001", "async main 必须返回 Unit、Result<Unit,String> 或 Result<Unit,Error>");
            } else if (concrete.result != Type::Unit) throw mismatch(checked, source, syntax.result.span, Type::Unit, concrete.result);
            checked.entry = checked.functions.size();
        }
        symbols.functionCallables[functionIndex] = symbols.callables.size();
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
    evaluateConstants(checked, sources);
    for (const auto& deferred : checked.deferredPatternChecks) {
        std::vector<const CheckedPattern*> patterns;
        for (const auto& pattern : deferred.patterns) patterns.push_back(&pattern);
        if (auto missing = missingPattern(checked, deferred.subject, patterns, deferred.use))
            throw uncoveredPattern(sources[deferred.file], deferred.site, deferred.use, *missing);
    }
    return checked;
}

}
