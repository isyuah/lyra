module;
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module lyra.macro_syntax;
import lyra.syntax;

export namespace lyra {

struct SyntaxRef { std::size_t index; bool operator==(const SyntaxRef&) const = default; };
enum class SyntaxForm { Code, Input, Argument, Join, Call, Member, Block, Around, Type };
struct SyntaxNode {
    SyntaxForm form;
    std::string kind, text;
    std::vector<SyntaxRef> children{};
    std::optional<Span> span{};
    std::string canonical{};
    std::string module{};
};
struct SyntaxPiece { std::string text; std::optional<Span> span{}; bool copied{}; };
struct SyntaxOutput { std::vector<SyntaxPiece> code; std::optional<std::vector<std::vector<SyntaxPiece>>> hooks{}; };

struct SyntaxArena {
    const Source& source;
    Span site;
    std::vector<SyntaxNode> nodes{};

    SyntaxRef add(SyntaxNode node) {
        auto ref = SyntaxRef{nodes.size()};
        nodes.push_back(std::move(node));
        return ref;
    }
    const SyntaxNode& get(SyntaxRef ref) const { return nodes[ref.index]; }
    std::string text(Span span) const { return source.text.substr(span.begin, span.end - span.begin); }
    SyntaxRef input(std::string kind, Span span, std::vector<SyntaxRef> children = {}, bool argument = false) {
        return add({argument ? SyntaxForm::Argument : SyntaxForm::Input, std::move(kind), {}, std::move(children), span});
    }

    SyntaxRef expression(const Function& fn, ExprId id) {
        const auto& expr = fn.expressions[id];
        std::vector<SyntaxRef> children;
        auto child = [&](ExprId value) { children.push_back(expression(fn, value)); };
        auto optional = [&](std::optional<ExprId> value) { if (value) child(*value); };
        auto named = [&](std::string kind, const Identifier& name) { children.push_back(input(std::move(kind), name.span)); };
        auto kind = std::visit([&](const auto& node) -> const char* {
            using Node = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<Node, Block>) {
                for (const auto& statement : node.statements) child(statement.expression);
                return "block";
            } else if constexpr (std::is_same_v<Node, Call>) {
                optional(node.receiver); optional(node.function);
                if (!node.function) named("name", node.name);
                for (const auto& argument : node.arguments) child(argument.value);
                return "call";
            } else if constexpr (std::is_same_v<Node, Binary>) {
                child(node.left); children.push_back(input("operator", node.site)); child(node.right); return "binary";
            } else if constexpr (std::is_same_v<Node, RangeValue>) {
                child(node.start); child(node.end); return node.inclusive ? "rangeInclusive" : "rangeExclusive";
            } else if constexpr (std::is_same_v<Node, Unary>) {
                children.push_back(input("operator", node.site)); child(node.operand); return "unary";
            } else if constexpr (std::is_same_v<Node, Return> || std::is_same_v<Node, Raise>) {
                optional(node.value); return std::is_same_v<Node, Return> ? "return" : "throw";
            } else if constexpr (std::is_same_v<Node, Binding>) {
                named("name", node.name); if (node.annotation) named("type", *node.annotation); child(node.value); return node.writable ? "var" : "val";
            } else if constexpr (std::is_same_v<Node, PatternBinding>) {
                children.push_back(input("pattern", node.pattern.span)); if (node.annotation) named("type", *node.annotation); child(node.value); return "val";
            } else if constexpr (std::is_same_v<Node, Conditional>) {
                child(node.condition); child(node.yes); optional(node.no); return "if";
            } else if constexpr (std::is_same_v<Node, While>) {
                child(node.condition); child(node.body); return "while";
            } else if constexpr (std::is_same_v<Node, For>) {
                named("name", node.item); child(node.input); child(node.body); return "for";
            } else if constexpr (std::is_same_v<Node, Member>) {
                child(node.receiver); named("name", node.name); return "member";
            } else if constexpr (std::is_same_v<Node, QualifiedMember>) {
                named("type", node.owner.name); named("name", node.name); return "member";
            } else if constexpr (std::is_same_v<Node, Index>) {
                child(node.receiver); child(node.offset); return "index";
            } else if constexpr (std::is_same_v<Node, Match>) {
                child(node.subject);
                for (const auto& arm : node.arms) { children.push_back(input("pattern", arm.pattern.span)); optional(arm.guard); child(arm.value); }
                return "match";
            } else if constexpr (std::is_same_v<Node, Select>) {
                for (const auto& arm : node.arms) {
                    if (arm.binding) named("name", *arm.binding);
                    child(arm.operation);
                    child(arm.value);
                }
                return "select";
            } else if constexpr (std::is_same_v<Node, Attempt>) {
                child(node.body);
                for (const auto& arm : node.arms) { if (arm.binding) named("type", arm.binding->type); optional(arm.guard); child(arm.body); }
                return "try";
            } else if constexpr (std::is_same_v<Node, Lambda>) {
                for (const auto& parameter : node.parameters) named("name", parameter.name);
                child(node.body); return "lambda";
            } else if constexpr (std::is_same_v<Node, TaskOp> || std::is_same_v<Node, Propagate>) {
                child(node.operand);
                if constexpr (std::is_same_v<Node, TaskOp>) return node.spawn ? "spawn" : "await";
                else return "propagate";
            } else if constexpr (std::is_same_v<Node, Blocking> || std::is_same_v<Node, Deferred> || std::is_same_v<Node, AsyncBlock>) {
                child(node.body);
                if constexpr (std::is_same_v<Node, Blocking>) return "blocking";
                else if constexpr (std::is_same_v<Node, Deferred>) return "defer";
                else return node.kind == AsyncBlock::Group ? "taskGroup" : "parallel";
            } else if constexpr (std::is_same_v<Node, TypeOperation>) {
                child(node.value); named("type", node.type); return "typeOperation";
            } else if constexpr (std::is_same_v<Node, TextTemplate>) {
                for (auto part : node.parts) child(part);
                return "template";
            } else if constexpr (std::is_same_v<Node, DisplayValue>) {
                child(node.value); return "display";
            } else if constexpr (std::is_same_v<Node, ListLiteral>) {
                for (auto element : node.elements) child(element);
                return "list";
            } else if constexpr (std::is_same_v<Node, Identifier>) return "name";
            else if constexpr (std::is_same_v<Node, IntegerLiteral> || std::is_same_v<Node, RealLiteral>) return "number";
            else if constexpr (std::is_same_v<Node, StringLiteral>) return "string";
            else if constexpr (std::is_same_v<Node, BytesLiteral>) return "bytes";
            else if constexpr (std::is_same_v<Node, CharLiteral>) return "char";
            else if constexpr (std::is_same_v<Node, bool>) return "bool";
            else return "expression";
        }, expr.node);
        return input(kind, expr.span, std::move(children));
    }

    SyntaxRef annotation(const Identifier& type) {
        return add({SyntaxForm::Input, "type", {}, {}, type.span, type.text});
    }

    SyntaxRef declaration(const Identifier& name, const std::vector<Field>& fields, const Function* fn, const Aggregate* aggregate, const Enumeration* enumeration, std::optional<SyntaxRef> body) {
        std::vector<SyntaxRef> children{input("name", name.span)};
        for (const auto& field : fields) {
            std::vector<SyntaxRef> signature{input("name", field.name.span), annotation(field.type)};
            for (const auto& mark : field.attributes) {
                std::vector<SyntaxRef> items;
                if (mark.argument) items.push_back(input("argument", mark.argument->span));
                signature.push_back(input("attribute", mark.name.span, std::move(items)));
            }
            children.push_back(input(aggregate ? "field" : "parameter", {field.name.span.begin, field.type.span.end}, std::move(signature)));
        }
        if (enumeration) for (const auto& entry : enumeration->cases) {
            std::vector<SyntaxRef> parts{input("name", entry.name.span)};
            auto span = entry.name.span;
            for (const auto& field : entry.fields) {
                std::vector<SyntaxRef> payload;
                if (field.name) payload.push_back(input("name", field.name->span));
                payload.push_back(annotation(field.type));
                parts.push_back(input("payload", {field.name ? field.name->span.begin : field.type.span.begin, field.type.span.end}, std::move(payload)));
                span.end = field.type.span.end;
            }
            children.push_back(input("case", span, std::move(parts)));
        }
        if (fn && !fn->result.text.empty()) children.push_back(annotation(fn->result));
        if (body) children.push_back(*body);
        auto span = enumeration ? enumeration->span : aggregate ? aggregate->span
            : Span{name.span.begin, body ? get(*body).span->end : fn->result.span.end};
        return input(enumeration ? "enum" : aggregate ? aggregate->reference ? "class" : "struct" : "fn", span, std::move(children));
    }

    void emit(SyntaxRef ref, std::vector<SyntaxPiece>& out, std::size_t& size, std::size_t& steps, unsigned depth = 0) const {
        if (++steps > 100000 || depth > 128) throw at(source, site, "LYM0002", "生成语法超过节点预算");
        auto append = [&](std::string value, std::optional<Span> span = {}, bool copied = false) {
            size += value.size();
            if (size > 1024 * 1024) throw at(source, site, "LYM0002", "生成语法超过字节预算");
            if (!value.empty()) out.push_back({std::move(value), span, copied});
        };
        const auto& node = get(ref);
        auto child = [&](std::size_t index) { emit(node.children[index], out, size, steps, depth + 1); };
        switch (node.form) {
            case SyntaxForm::Code: append(node.text, node.span); break;
            case SyntaxForm::Input: append(text(*node.span), node.span, true); break;
            case SyntaxForm::Argument: append("(", node.span); append(text(*node.span), node.span, true); append("\n)", node.span); break;
            case SyntaxForm::Join: child(0); append(node.text); child(1); break;
            case SyntaxForm::Call: child(0); append("("); child(1); append("\n)"); break;
            case SyntaxForm::Member: child(0); append("." + node.text); break;
            case SyntaxForm::Block: append("{\n"); child(0); append("\n}"); break;
            case SyntaxForm::Around: throw at(source, site, "LYM0002", "syntaxAround 只能作为装饰器的整个输出");
            case SyntaxForm::Type: throw at(source, site, "LYM0002", "已解析类型只能查询，不能当作源码输出");
        }
    }

    std::vector<SyntaxPiece> render(SyntaxRef ref) const {
        std::size_t size = 0, steps = 0;
        std::vector<SyntaxPiece> out;
        emit(ref, out, size, steps);
        return out;
    }
};

}
