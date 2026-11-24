module;
#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <span>
#include <string>
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

std::string describeRequirement(const std::string& name, const RequiredMethod& method, const auto& typeName) {
    std::string text = method.asynchronous ? "async fn " : "fn ";
    text += name + '(';
    for (std::size_t index = 0; index < method.parameters.size(); ++index) {
        if (index) text += ", ";
        text += method.labels[index] + ": " + typeName(method.parameters[index]);
    }
    return text + "): " + typeName(method.result);
}

class StructuralContracts {
    std::map<std::string, const Trait*> traits;
    std::map<std::size_t, std::vector<std::string>> variables;

public:
    void declare(const Trait& trait, const Source& source) {
        if (!traits.emplace(trait.name.text, &trait).second) throw at(source, trait.name.span, "LYN0002", "结构约束重复：" + trait.name.text);
        std::set<std::string> associated, methods;
        for (const auto& type : trait.associated)
            if (!associated.insert(type.text).second) throw at(source, type.span, "LYN0002", "关联类型重复：" + type.text);
        for (const auto& method : trait.methods) {
            if (!methods.insert(method.name.text).second) throw at(source, method.name.span, "LYN0002", "约束方法重复：" + method.name.text);
            std::set<std::string> parameters;
            for (const auto& parameter : method.parameters)
                if (!parameters.insert(parameter.name.text).second) throw at(source, parameter.name.span, "LYN0002", "约束方法形参重复：" + parameter.name.text);
        }
    }

    void bind(Type variable, const Identifier& bound, const Source& source) {
        if (!traits.contains(bound.text)) throw at(source, bound.span, "LYG0005", "找不到结构约束 trait：" + bound.text);
        auto& constraints = variables[variable.id];
        if (std::ranges::find(constraints, bound.text) != constraints.end())
            throw at(source, bound.span, "LYN0002", "结构约束重复：" + bound.text);
        constraints.push_back(bound.text);
    }

    const Trait& declaration(const std::string& name) const { return *traits.at(name); }

    std::span<const std::string> constraints(Type variable) const {
        if (variable.kind != Type::Variable) return {};
        auto found = variables.find(variable.id);
        return found == variables.end() ? std::span<const std::string>{} : found->second;
    }
};
}
