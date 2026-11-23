module;
#include <compare>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

struct TypeForm { std::string name; std::vector<TypeForm> arguments; };

TypeForm typeForm(std::string_view canonical) {
    std::size_t pos = 0;
    auto parse = [&](auto&& self) -> TypeForm {
        auto begin = pos;
        while (pos < canonical.size() && canonical[pos] != '<' && canonical[pos] != '>' && canonical[pos] != ',') ++pos;
        if (pos == begin) throw std::logic_error("empty name in canonical type");
        TypeForm result{std::string(canonical.substr(begin, pos - begin)), {}};
        if (pos < canonical.size() && canonical[pos] == '<') {
            ++pos;
            for (;;) {
                result.arguments.push_back(self(self));
                if (pos >= canonical.size()) throw std::logic_error("unclosed canonical type");
                if (canonical[pos++] == '>') break;
            }
        }
        return result;
    };
    auto result = parse(parse);
    if (pos != canonical.size()) throw std::logic_error("trailing canonical type");
    return result;
}

template<class Rename> std::string mapTypeNames(const TypeForm& form, const Rename& rename) {
    auto result = rename(form.name);
    if (!form.arguments.empty()) {
        result += '<';
        for (std::size_t index = 0; index < form.arguments.size(); ++index) {
            if (index) result += ',';
            result += mapTypeNames(form.arguments[index], rename);
        }
        result += '>';
    }
    return result;
}

struct GenericPattern { std::size_t parameter, taskDepth; };
std::optional<GenericPattern> genericPattern(const Function& declaration, const std::string& annotation) {
    auto form = typeForm(annotation); const TypeForm* current = &form; std::size_t depth = 0;
    while (current->name == "Task" && current->arguments.size() == 1) { current = &current->arguments[0]; ++depth; }
    if (!current->arguments.empty()) return std::nullopt;
    if (auto parameter = genericParameter(declaration, current->name)) return GenericPattern{*parameter, depth};
    return std::nullopt;
}

template<class Type> struct GenericArgument { std::size_t slot; Type type; Span span; };

template<class Type, class TypeName, class UnwrapTask>
std::vector<Type> inferGenericArguments(const Function& declaration, const Call& call, Span site,
        std::span<const Type> explicitTypes, std::span<const GenericArgument<Type>> arguments,
        std::optional<Type> expected, const Source& source, TypeName name, UnwrapTask unwrapTask) {
    const auto& parameters = declaration.typeParameters;
    if (!explicitTypes.empty() && explicitTypes.size() != parameters.size()) {
        throw at(source, call.name.span, "LYG0001", call.name.text + " 需要 " + std::to_string(parameters.size()) + " 个类型实参，实际为 " + std::to_string(explicitTypes.size()));
    }
    std::vector<std::optional<Type>> bound(parameters.size());
    for (std::size_t index = 0; index < explicitTypes.size(); ++index) bound[index] = explicitTypes[index];
    auto inner = [&](Type value, std::size_t depth) -> std::optional<Type> {
        for (std::size_t step = 0; step < depth; ++step) {
            auto element = unwrapTask(value);
            if (!element) return std::nullopt;
            value = *element;
        }
        return value;
    };
    for (const auto& argument : arguments) {
        if (argument.type == Type::Never) continue;
        const auto& annotation = declaration.parameters[argument.slot].type.text;
        auto pattern = genericPattern(declaration, annotation);
        if (!pattern) continue;
        auto element = inner(argument.type, pattern->taskDepth);
        if (!element) {
            auto error = at(source, argument.span, "LYT0001", "期望 " + annotation + "，实际是 " + name(argument.type));
            error.expected = annotation; error.actual = name(argument.type); throw error;
        }
        auto index = pattern->parameter;
        if (bound[index] && *bound[index] != *element) {
            auto previous = name(*bound[index]); auto actual = name(*element);
            auto error = at(source, argument.span, "LYT0001", "类型参数 " + parameters[index].text + " 已确定为 " + previous + "，此实参为 " + actual);
            error.expected = previous; error.actual = actual;
            throw error;
        }
        bound[index] = *element;
    }
    auto result = genericPattern(declaration, declaration.result.text);
    if (result && !bound[result->parameter] && expected && *expected != Type::Never) bound[result->parameter] = inner(*expected, result->taskDepth);
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
    auto substitute = [&](Identifier& type) {
        type.text = mapTypeNames(typeForm(type.text), [&](const std::string& name) { auto index = genericParameter(declaration, name); return index ? types[*index] : name; });
    };
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
