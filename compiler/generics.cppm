module;
#include <compare>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

export module lyra.generics;
import lyra.syntax;

export namespace lyra {

std::optional<std::size_t> genericParameter(const Function& declaration, const std::string& name) {
    for (std::size_t index = 0; index < declaration.typeParameters.size(); ++index) {
        if (declaration.typeParameters[index].text == name) return index;
    }
    return std::nullopt;
}

template<class Type> struct GenericArgument { std::size_t slot; Type type; Span span; };

template<class Type, class TypeName>
std::vector<Type> inferGenericArguments(const Function& declaration, const Call& call, Span site,
        std::span<const Type> explicitTypes, std::span<const GenericArgument<Type>> arguments,
        std::optional<Type> expected, const Source& source, TypeName name) {
    const auto& parameters = declaration.typeParameters;
    if (!explicitTypes.empty() && explicitTypes.size() != parameters.size()) {
        throw at(source, call.name.span, "LYG0001", call.name.text + " 需要 " + std::to_string(parameters.size()) + " 个类型实参，实际为 " + std::to_string(explicitTypes.size()));
    }
    std::vector<std::optional<Type>> bound(parameters.size());
    for (std::size_t index = 0; index < explicitTypes.size(); ++index) bound[index] = explicitTypes[index];
    for (const auto& argument : arguments) {
        if (argument.type == Type::Never) continue;
        auto index = genericParameter(declaration, declaration.parameters[argument.slot].type.text);
        if (!index) continue;
        if (bound[*index] && *bound[*index] != argument.type) {
            auto previous = name(*bound[*index]); auto actual = name(argument.type);
            auto error = at(source, argument.span, "LYT0001", "类型参数 " + parameters[*index].text + " 已确定为 " + previous + "，此实参为 " + actual);
            error.expected = previous; error.actual = actual;
            throw error;
        }
        bound[*index] = argument.type;
    }
    auto result = genericParameter(declaration, declaration.result.text);
    if (result && !bound[*result] && expected && *expected != Type::Never) bound[*result] = *expected;
    std::vector<Type> types;
    for (std::size_t index = 0; index < bound.size(); ++index) {
        if (!bound[index]) {
            auto error = at(source, site, "LYG0002", "调用 " + call.name.text + " 时无法推断类型参数 " + parameters[index].text);
            error.hint = "在函数名后写出类型实参，或给调用结果明确的目标类型。";
            throw error;
        }
        types.push_back(*bound[index]);
    }
    return types;
}

struct GenericInstanceKey {
    std::size_t declaration;
    std::vector<std::string> arguments;
    auto operator<=>(const GenericInstanceKey&) const = default;
};

Function specializeGeneric(const Function& declaration, const std::vector<std::string>& types) {
    auto result = declaration;
    auto substitute = [&](Identifier& type) { if (auto index = genericParameter(declaration, type.text)) type.text = types[*index]; };
    substitute(result.result);
    for (auto& parameter : result.parameters) substitute(parameter.type);
    for (auto& expression : result.expressions) {
        if (auto binding = std::get_if<Binding>(&expression.node); binding && binding->annotation) substitute(*binding->annotation);
        if (auto call = std::get_if<Call>(&expression.node)) for (auto& type : call->typeArguments) substitute(type);
    }
    result.name.text += '<';
    for (std::size_t index = 0; index < types.size(); ++index) {
        if (index) result.name.text += ',';
        result.name.text += types[index];
    }
    result.name.text += '>';
    result.typeParameters.clear();
    return result;
}

}
