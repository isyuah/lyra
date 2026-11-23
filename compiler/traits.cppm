module;
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

export module lyra.traits;
import lyra.check;

export namespace lyra {

struct RequiredMethod {
    std::vector<std::string> labels;
    std::vector<Type> parameters;
    Type result;
    bool asynchronous;
    bool operator==(const RequiredMethod&) const = default;
};

class StructuralContracts {
    using Shape = std::map<std::string, RequiredMethod>;
    std::map<std::string, Shape> traits;
    std::map<std::size_t, const Shape*> variables;

    static std::string describe(const std::string& name, const RequiredMethod& method, const auto& typeName) {
        std::string text = method.asynchronous ? "async fn " : "fn ";
        text += name + '(';
        for (std::size_t index = 0; index < method.parameters.size(); ++index) {
            if (index) text += ", ";
            text += method.labels[index] + ": " + typeName(method.parameters[index]);
        }
        return text + "): " + typeName(method.result);
    }

public:
    void define(const Trait& trait, const Source& source, const auto& resolve) {
        auto [entry, inserted] = traits.try_emplace(trait.name.text);
        if (!inserted) throw at(source, trait.name.span, "LYN0002", "结构约束重复：" + trait.name.text);
        for (const auto& method : trait.methods) {
            RequiredMethod shape{{}, {}, resolve(method.result), method.asynchronous};
            std::set<std::string> names;
            for (const auto& parameter : method.parameters) {
                if (!names.insert(parameter.name.text).second) throw at(source, parameter.name.span, "LYN0002", "约束方法形参重复：" + parameter.name.text);
                shape.labels.push_back(parameter.name.text);
                shape.parameters.push_back(resolve(parameter.type));
            }
            if (!entry->second.emplace(method.name.text, std::move(shape)).second) throw at(source, method.name.span, "LYN0002", "约束方法重复：" + method.name.text);
        }
    }

    void bind(Type variable, const Identifier& bound, const Source& source) {
        auto trait = traits.find(bound.text);
        if (trait == traits.end()) throw at(source, bound.span, "LYG0005", "找不到结构约束 trait：" + bound.text);
        variables.emplace(variable.id, &trait->second);
    }

    const RequiredMethod* member(Type variable, const std::string& name) const {
        auto bound = variables.find(variable.id);
        if (bound == variables.end()) return nullptr;
        auto found = bound->second->find(name);
        return found == bound->second->end() ? nullptr : &found->second;
    }

    void require(const Function& target, std::span<const Type> arguments, const Source& use, Span site, const auto& concrete, const auto& typeName) const {
        for (const auto& bound : target.bounds) {
            auto actual = arguments[bound.parameter];
            for (const auto& [name, expected] : traits.at(bound.trait.text)) {
                std::optional<RequiredMethod> found;
                if (actual.kind == Type::Variable) {
                    if (auto method = member(actual, name)) found = *method;
                } else found = concrete(actual, name);
                if (found && *found == expected) continue;
                auto reason = found ? "，实际为 " + describe(name, *found, typeName) : "，缺少可见方法 " + name;
                throw at(use, site, "LYG0005", typeName(actual) + " 不满足 " + bound.trait.text + "：需要 " + describe(name, expected, typeName) + reason);
            }
        }
    }
};
}
