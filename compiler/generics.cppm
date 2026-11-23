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

template<class Type> struct GenericArgument { std::size_t slot; Type type; Span span; };

template<class Type, class TypeName, class Project, class Common>
std::vector<Type> inferGenericArguments(const Function& declaration, const Call& call, Span site,
        std::span<const Type> explicitTypes, std::span<const GenericArgument<Type>> arguments,
        std::optional<Type> expected, const Source& source, TypeName name, Project project, Common common) {
    const auto& parameters = declaration.typeParameters;
    std::vector<std::optional<Type>> bound(parameters.size());
    for (std::size_t index = 0; index < explicitTypes.size(); ++index) bound[index] = explicitTypes[index];
    auto unify = [&](auto&& self, const TypeForm& form, Type value, Span span, bool fromResult, bool widen) -> void {
        if (auto index = genericParameter(declaration, form.name)) {
            if (!bound[*index]) bound[*index] = value;
            else if (!fromResult && *bound[*index] != value) {
                if (widen && *index >= explicitTypes.size()) {
                    if (auto joined = common(*bound[*index], value)) { bound[*index] = *joined; return; }
                }
                auto previous = name(*bound[*index]); auto actual = name(value);
                auto error = at(source, span, "LYT0001", "类型参数 " + parameters[*index].text + " 已确定为 " + previous + "，此实参为 " + actual);
                error.expected = previous; error.actual = actual; throw error;
            }
            return;
        }
        for (std::size_t slot = 0; slot < form.arguments.size(); ++slot) {
            auto element = project(value, form.name, slot);
            if (!element && !fromResult && form.name == "Option") element = value;
            if (element) self(self, form.arguments[slot], *element, span, fromResult, false);
            else if (!fromResult) {
                auto annotation = mapTypeNames(form, [](const auto& part) { return part; });
                auto error = at(source, span, "LYT0001", "期望 " + annotation + "，实际是 " + name(value));
                error.expected = annotation; error.actual = name(value); throw error;
            }
        }
    };
    for (const auto& argument : arguments) {
        if (argument.type == Type::Never) continue;
        unify(unify, typeForm(declaration.parameters[argument.slot].type.text), argument.type, argument.span, false, true);
    }
    if (expected && *expected != Type::Never && !declaration.result.text.empty()) unify(unify, typeForm(declaration.result.text), *expected, site, true, false);
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

Function specializeGeneric(Function result, const std::vector<std::string>& types) {
    auto substitute = [&](Identifier& type) {
        type.text = mapTypeNames(typeForm(type.text), [&](const std::string& name) { auto index = genericParameter(result, name); return index ? types[*index] : name; });
    };
    substitute(result.result);
    for (auto& parameter : result.parameters) substitute(parameter.type);
    for (auto& expression : result.expressions) {
        if (auto binding = std::get_if<Binding>(&expression.node); binding && binding->annotation) substitute(*binding->annotation);
        if (auto attempt = std::get_if<Attempt>(&expression.node); attempt && attempt->binding) substitute(attempt->binding->type);
        if (auto lambda = std::get_if<Lambda>(&expression.node)) {
            for (auto& param : lambda->parameters) if (param.type) substitute(*param.type);
            if (lambda->result) substitute(*lambda->result);
        }
        if (auto member = std::get_if<QualifiedMember>(&expression.node)) for (auto& type : member->owner.arguments) substitute(type);
        if (auto call = std::get_if<Call>(&expression.node)) {
            if (call->typeOwner) for (auto& type : call->typeOwner->arguments) substitute(type);
            for (auto& type : call->typeArguments) substitute(type);
        }
    }
    result.name.text += '<';
    for (std::size_t index = 0; index < types.size(); ++index) {
        if (index) result.name.text += ',';
        result.name.text += types[index];
    }
    result.name.text += '>';
    result.typeParameters.clear();
    result.bounds.clear();
    return result;
}

}
