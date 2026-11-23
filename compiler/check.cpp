#include "frontend.hpp"

#include <unordered_map>

namespace lyra {

const char* typeName(Type type) { return type == Type::Int ? "Int" : "Unit"; }

static Diagnostic mismatch(const Source& source, Span span, Type expected, Type actual) {
    auto error = at(source, span, "LYT0001", std::string("期望 ") + typeName(expected) + "，实际是 " + typeName(actual));
    error.expected = typeName(expected);
    error.actual = typeName(actual);
    return error;
}

void check(const Module& functions, const std::vector<Source>& sources) {
    std::unordered_map<std::string, Type> results{{"print", Type::Unit}};
    for (const auto& function : functions) {
        const auto& source = sources[function.file];
        if (!results.emplace(function.name, function.result).second) {
            throw at(source, function.nameSpan, "LYN0002", "函数名已声明：" + function.name);
        }
        if (function.name == "main" && function.result != Type::Unit) {
            throw mismatch(source, function.resultSpan, Type::Unit, function.result);
        }
    }
    for (const auto& function : functions) {
        const auto& source = sources[function.file];
        std::vector<Type> types;
        for (const auto& expr : function.expressions) {
            Type type = Type::Int;
            if (auto* call = std::get_if<Call>(&expr.node)) {
                auto found = results.find(call->name);
                if (found == results.end()) throw at(source, call->nameSpan, "LYN0001", "找不到函数：" + call->name);
                std::size_t arity = call->name == "print" ? 1 : 0;
                if (call->arguments.size() != arity) {
                    throw at(source, call->nameSpan, "LYT0002", call->name + " 需要 " + std::to_string(arity) + " 个实参，实际为 " + std::to_string(call->arguments.size()));
                }
                for (auto arg : call->arguments) {
                    if (types[arg] != Type::Int) throw mismatch(source, function.expressions[arg].span, Type::Int, types[arg]);
                }
                type = found->second;
            } else if (auto* add = std::get_if<Add>(&expr.node)) {
                for (auto operand : {add->left, add->right}) {
                    if (types[operand] != Type::Int) throw mismatch(source, function.expressions[operand].span, Type::Int, types[operand]);
                }
            }
            types.push_back(type);
        }
        Type tailType = Type::Unit;
        Span tailSpan = function.closingBrace;
        bool removingSemiFixes = false;
        if (!function.body.empty()) {
            const auto& tail = function.body.back();
            tailSpan = tail.semicolon.value_or(function.expressions[tail.expression].span);
            if (!tail.semicolon) tailType = types[tail.expression];
            else removingSemiFixes = types[tail.expression] == function.result;
        }
        if (tailType != function.result) {
            auto error = mismatch(source, tailSpan, function.result, tailType);
            if (removingSemiFixes) error.hint = "去掉末尾分号，让最后的表达式成为返回值。";
            throw error;
        }
    }
    if (!results.contains("main")) throw Diagnostic{"LYN0001", "可执行包缺少入口 fn main(): Unit"};
}

}
