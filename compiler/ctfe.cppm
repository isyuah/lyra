module;
#include <cstdio>
#include <cstdint>
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <lyra/utf8.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module lyra.ctfe;
import lyra.check;
import lyra.numbers;

export namespace lyra {
void evaluateConstants(CheckedModule& module, const std::vector<Source>& sources);
}

namespace lyra {
namespace {

bool scalar(Type type) {
    return numericType(type) || type == Type::Bool || type == Type::String || type == Type::Bytes ||
        type == Type::Char || type == Type::Unit || type == Type::Never;
}

Type rawType(const CheckedFunction& fn, ExprId id) {
    const auto& expr = fn.expressions[id];
    return expr.sourceType.value_or(expr.type);
}

[[noreturn]] void error(const std::vector<Source>& sources, const CheckedFunction& fn, Span site, const char* code, std::string message) {
    throw at(sources[fn.syntax.file], site, code, std::move(message));
}

struct Validator {
    const CheckedModule& module;
    const std::vector<Source>& sources;
    const CheckedFunction& fn;

    [[noreturn]] void reject(Span site, std::string detail) const {
        error(sources, fn, site, "LYCT0001", std::move(detail));
    }

    void visit(ExprId id) const {
        const auto& expr = fn.syntax.expressions[id];
        const auto& info = fn.expressions[id];
        if (info.conversion) reject(expr.span, "编译期计算不能调用用户隐式转换");
        if (!scalar(info.type) || !scalar(rawType(fn, id))) reject(expr.span, "编译期计算只支持标量和文本");
        const auto descend = [&](ExprId child) { visit(child); };
        std::visit([&](const auto& node) {
            using Node = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<Node, Call>) {
                if (node.receiver) descend(*node.receiver);
                for (const auto& arg : node.arguments) descend(arg.value);
                if (info.callee == Callee::ToString || info.callee == Callee::ToReal || info.callee == Callee::ToBytes) return;
                if (!node.receiver && !node.function && info.callee == Callee::Function &&
                    module.functions[info.target].syntax.constant == ConstKind::Function) return;
                reject(node.name.span, "编译期只能调用 const fn 和纯标量操作：" + node.name.text);
            } else if constexpr (std::is_same_v<Node, Binary>) {
                if (node.op <= Op::RemSet && !std::holds_alternative<Identifier>(fn.syntax.expressions[node.left].node))
                    reject(node.site, "编译期只能写入局部 var");
                descend(node.left);
                descend(node.right);
            } else if constexpr (std::is_same_v<Node, Binding> || std::is_same_v<Node, DisplayValue>) descend(node.value);
            else if constexpr (std::is_same_v<Node, Unary>) descend(node.operand);
            else if constexpr (std::is_same_v<Node, Member>) descend(node.receiver);
            else if constexpr (std::is_same_v<Node, TextTemplate>) { for (auto part : node.parts) descend(part); }
            else if constexpr (std::is_same_v<Node, Block>) { for (const auto& statement : node.statements) descend(statement.expression); }
            else if constexpr (std::is_same_v<Node, Conditional>) {
                descend(node.condition);
                descend(node.yes);
                if (node.no) descend(*node.no);
            } else if constexpr (std::is_same_v<Node, While>) {
                descend(node.condition);
                descend(node.body);
            } else if constexpr (std::is_same_v<Node, For>) {
                if (!node.range) reject(expr.span, "编译期 for 只支持整数区间");
                descend(node.input);
                descend(node.range->end);
                descend(node.body);
            } else if constexpr (std::is_same_v<Node, Return>) { if (node.value) descend(*node.value); }
            else if constexpr (!(std::is_same_v<Node, Identifier> || std::is_same_v<Node, Jump> || std::is_same_v<Node, bool> ||
                std::is_same_v<Node, IntegerLiteral> || std::is_same_v<Node, RealLiteral> || std::is_same_v<Node, CharLiteral> ||
                std::is_same_v<Node, StringLiteral> || std::is_same_v<Node, BytesLiteral>)) reject(expr.span, "编译期计算不支持此表达式");
        }, expr.node);
    }

    void run() const {
        if (!scalar(fn.result)) reject(fn.syntax.result.span, "编译期结果必须是标量或文本");
        for (std::size_t slot = 0; slot < fn.syntax.parameters.size(); ++slot)
            if (!scalar(fn.locals[slot].type)) reject(fn.syntax.parameters[slot].type.span, "const fn 形参必须是标量或文本");
        visit(fn.syntax.body);
    }
};

struct Returned { ConstScalar value; };

struct Evaluation {
    CheckedModule& module;
    const std::vector<Source>& sources;
    std::vector<bool>& active;
    unsigned remaining = 100000, depth = 0;
    std::size_t bytes = 0;

    static llvm::APFloat real(Type type, double value) {
        return numberWidth(type) == 32 ? llvm::APFloat(static_cast<float>(value)) : llvm::APFloat(value);
    }

    static double asDouble(const llvm::APFloat& value, Type type) {
        return numberWidth(type) == 32 ? static_cast<double>(value.convertToFloat()) : value.convertToDouble();
    }

    static llvm::APInt integer(Type type, const ConstScalar& value) {
        return llvm::APInt(numberWidth(type), std::get<std::uint64_t>(value));
    }

    void charge(const CheckedFunction& fn, Span site, std::size_t size) {
        bytes += size;
        if (size > 1024 * 1024 || bytes > 32 * 1024 * 1024) error(sources, fn, site, "LYCT0004", "编译期文本超出字节预算");
    }

    ConstScalar invoke(const CheckedFunction& fn, std::vector<ConstScalar> locals, const CheckedFunction& caller, Span call) {
        if (++depth > 64) error(sources, caller, call, "LYCT0004", "编译期调用深度超出预算（64）");
        locals.resize(fn.locals.size());
        ConstScalar result;
        try { result = eval(fn, fn.syntax.body, locals); }
        catch (Returned& returned) { result = std::move(returned.value); }
        --depth;
        return result;
    }

    const ConstScalar& value(std::size_t index, const CheckedFunction& caller, Span site) {
        auto& cached = module.constants[index];
        if (cached) return *cached;
        const auto& fn = module.functions[index];
        if (active[index]) error(sources, caller, site, "LYCT0002", "编译期常量存在依赖环：" + fn.syntax.name.text);
        active[index] = true;
        cached = invoke(fn, {}, caller, site);
        active[index] = false;
        return *cached;
    }

    static std::string display(ConstScalar value, Type type) {
        switch (type.kind) {
            case Type::String: return std::get<std::string>(std::move(value));
            case Type::Bool: return std::get<bool>(value) ? "true" : "false";
            case Type::Int: return std::to_string(integer(type, value).getSExtValue());
            case Type::UInt: return std::to_string(std::get<std::uint64_t>(value));
            case Type::Char: {
                char encoded[4];
                return std::string(encoded, utf8::encode(static_cast<std::uint32_t>(std::get<std::uint64_t>(value)), encoded));
            }
            case Type::Real: {
                char buffer[64];
                auto size = std::snprintf(buffer, sizeof buffer, numberWidth(type) == 32 ? "%.9g" : "%.17g", std::get<double>(value));
                return std::string(buffer, static_cast<std::size_t>(size));
            }
            default: std::unreachable();
        }
    }

    ConstScalar operation(const CheckedFunction& fn, Span site, Op op, Type type, const ConstScalar& left, const ConstScalar& right) {
        if (type == Type::String && op == Op::Add) return std::get<std::string>(left) + std::get<std::string>(right);
        if (!numericType(type)) return op == Op::Eq ? left == right : left != right;
        if (type.kind == Type::Real) {
            auto first = real(type, std::get<double>(left)), second = real(type, std::get<double>(right));
            constexpr auto mode = llvm::APFloat::rmNearestTiesToEven;
            auto order = first.compare(second);
            switch (op) {
                case Op::Eq: return order == llvm::APFloat::cmpEqual;
                case Op::Ne: return order != llvm::APFloat::cmpEqual;
                case Op::Lt: return order == llvm::APFloat::cmpLessThan;
                case Op::Le: return order == llvm::APFloat::cmpLessThan || order == llvm::APFloat::cmpEqual;
                case Op::Gt: return order == llvm::APFloat::cmpGreaterThan;
                case Op::Ge: return order == llvm::APFloat::cmpGreaterThan || order == llvm::APFloat::cmpEqual;
                case Op::Add: first.add(second, mode); break;
                case Op::Sub: first.subtract(second, mode); break;
                case Op::Mul: first.multiply(second, mode); break;
                case Op::Div: first.divide(second, mode); break;
                case Op::Rem: first.mod(second); break;
                default: std::unreachable();
            }
            return asDouble(first, type);
        }
        auto first = integer(type, left), second = integer(type, right);
        bool sign = type.kind == Type::Int, overflow = false;
        llvm::APInt result;
        switch (op) {
            case Op::Eq: return first == second;
            case Op::Ne: return first != second;
            case Op::Lt: return sign ? first.slt(second) : first.ult(second);
            case Op::Le: return sign ? first.sle(second) : first.ule(second);
            case Op::Gt: return sign ? first.sgt(second) : first.ugt(second);
            case Op::Ge: return sign ? first.sge(second) : first.uge(second);
            case Op::Add: result = sign ? first.sadd_ov(second, overflow) : first.uadd_ov(second, overflow); break;
            case Op::Sub: result = sign ? first.ssub_ov(second, overflow) : first.usub_ov(second, overflow); break;
            case Op::Mul: result = sign ? first.smul_ov(second, overflow) : first.umul_ov(second, overflow); break;
            case Op::Div: case Op::Rem:
                if (second.isZero()) error(sources, fn, site, "LYCT0003", "编译期整数除零");
                if (op == Op::Div) result = sign ? first.sdiv_ov(second, overflow) : first.udiv(second);
                else if (sign && second.isAllOnes()) return std::uint64_t{0};
                else result = sign ? first.srem(second) : first.urem(second);
                break;
            default: std::unreachable();
        }
        if (overflow) error(sources, fn, site, "LYCT0003", "编译期整数运算超出 " + numberName(type) + " 的范围");
        return result.getZExtValue();
    }

    ConstScalar eval(const CheckedFunction& fn, ExprId id, std::vector<ConstScalar>& locals) {
        const auto& info = fn.expressions[id];
        const auto& [site, node] = fn.syntax.expressions[id];
        if (!remaining--) error(sources, fn, site, "LYCT0004", "编译期计算超出执行步数预算");
        auto sub = [&](ExprId child) { return eval(fn, child, locals); };
        auto result = std::visit([&](const auto& item) -> ConstScalar {
            using Node = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Node, IntegerLiteral>) {
                if (info.realLiteral) return *info.realLiteral;
                return llvm::APInt(numberWidth(rawType(fn, id)), integerBits(item)).getZExtValue();
            } else if constexpr (std::is_same_v<Node, RealLiteral>) return *info.realLiteral;
            else if constexpr (std::is_same_v<Node, bool>) return item;
            else if constexpr (std::is_same_v<Node, CharLiteral>) return static_cast<std::uint64_t>(item.point);
            else if constexpr (std::is_same_v<Node, StringLiteral> || std::is_same_v<Node, BytesLiteral>) return item.bytes;
            else if constexpr (std::is_same_v<Node, Identifier>) return info.constantRef ? value(*info.constantRef, fn, site) : locals[info.target];
            else if constexpr (std::is_same_v<Node, Binding>) locals[info.target] = sub(item.value);
            else if constexpr (std::is_same_v<Node, DisplayValue>) return display(sub(item.value), fn.expressions[item.value].type);
            else if constexpr (std::is_same_v<Node, TextTemplate>) {
                std::string combined;
                for (auto part : item.parts) {
                    combined += std::get<std::string>(sub(part));
                    if (combined.size() > 1024 * 1024) charge(fn, site, combined.size());
                }
                return combined;
            } else if constexpr (std::is_same_v<Node, Member>) {
                auto receiver = sub(item.receiver);
                if (item.name.text == "codePoint") return receiver;
                auto& text = std::get<std::string>(receiver);
                if (item.name.text == "byteSize" || fn.expressions[item.receiver].type == Type::Bytes) return static_cast<std::uint64_t>(text.size());
                std::uint64_t count = 0;
                for (std::size_t offset = 0; offset < text.size(); ++count) offset += utf8::sequenceWidth(static_cast<unsigned char>(text[offset]));
                return count;
            } else if constexpr (std::is_same_v<Node, Block>) {
                ConstScalar tail;
                for (const auto& statement : item.statements) tail = sub(statement.expression);
                return rawType(fn, id) == Type::Unit ? ConstScalar{} : std::move(tail);
            } else if constexpr (std::is_same_v<Node, Conditional>) {
                ConstScalar branch;
                if (std::get<bool>(sub(item.condition))) branch = sub(item.yes);
                else if (item.no) branch = sub(*item.no);
                return rawType(fn, id) == Type::Unit ? ConstScalar{} : std::move(branch);
            } else if constexpr (std::is_same_v<Node, While>) {
                while (std::get<bool>(sub(item.condition))) {
                    try { sub(item.body); } catch (Jump jump) { if (jump == Jump::Break) break; }
                }
            } else if constexpr (std::is_same_v<Node, For>) {
                auto start = integer(Type::Int, sub(item.input)).getSExtValue();
                auto end = integer(Type::Int, sub(item.range->end)).getSExtValue();
                for (auto current = start; current < end || (item.range->inclusive && current == end); ++current) {
                    locals[info.target] = static_cast<std::uint64_t>(current);
                    try { sub(item.body); } catch (Jump jump) { if (jump == Jump::Break) break; }
                    if (current == end) break;
                }
            } else if constexpr (std::is_same_v<Node, Jump>) throw item;
            else if constexpr (std::is_same_v<Node, Return>) throw Returned{item.value ? sub(*item.value) : ConstScalar{}};
            else if constexpr (std::is_same_v<Node, Unary>) {
                auto operand = sub(item.operand);
                if (item.op == Op::Not) return !std::get<bool>(operand);
                auto type = fn.expressions[item.operand].type;
                if (type.kind == Type::Real) return -std::get<double>(operand);
                return operation(fn, item.site, Op::Sub, type, std::uint64_t{0}, operand);
            } else if constexpr (std::is_same_v<Node, Binary>) {
                auto op = item.op;
                auto type = fn.expressions[item.left].type;
                if (op <= Op::RemSet) {
                    if (type == Type::Never) return sub(item.left);
                    auto slot = fn.expressions[item.left].target;
                    auto before = op == Op::Set ? ConstScalar{} : locals[slot];
                    auto after = sub(item.right);
                    constexpr Op compound[] = {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Rem};
                    if (op != Op::Set) {
                        after = operation(fn, item.site, compound[static_cast<int>(op) - 1], type, before, after);
                        if (auto* text = std::get_if<std::string>(&after)) charge(fn, site, text->size());
                    }
                    locals[slot] = std::move(after);
                } else {
                    auto left = sub(item.left);
                    if (op == Op::And) return std::get<bool>(left) && std::get<bool>(sub(item.right));
                    if (op == Op::Or) return std::get<bool>(left) || std::get<bool>(sub(item.right));
                    auto right = sub(item.right);
                    return operation(fn, item.site, op, type, left, right);
                }
            } else if constexpr (std::is_same_v<Node, Call>) {
                if (item.receiver) {
                    auto receiver = sub(*item.receiver);
                    auto from = fn.expressions[*item.receiver].type;
                    if (info.callee == Callee::ToString) return display(std::move(receiver), from);
                    if (info.callee == Callee::ToBytes) return receiver;
                    auto target = rawType(fn, id);
                    if (from.kind == Type::Real) return asDouble(real(target, std::get<double>(receiver)), target);
                    llvm::APFloat result(numberWidth(target) == 32 ? llvm::APFloat::IEEEsingle() : llvm::APFloat::IEEEdouble());
                    result.convertFromAPInt(integer(from, receiver), from.kind == Type::Int, llvm::APFloat::rmNearestTiesToEven);
                    return asDouble(result, target);
                }
                const auto& target = module.functions[info.target];
                std::vector<ConstScalar> args(target.syntax.parameters.size());
                for (std::size_t index = 0; index < item.arguments.size(); ++index) {
                    auto evaluated = sub(item.arguments[index].value);
                    args[info.argumentSlots[index]] = std::move(evaluated);
                }
                return invoke(target, std::move(args), fn, item.name.span);
            } else std::unreachable();
            return {};
        }, node);
        if (auto* text = std::get_if<std::string>(&result)) charge(fn, site, text->size());
        if (info.sourceType && integerType(*info.sourceType)) {
            auto bits = integer(*info.sourceType, result);
            return info.sourceType->kind == Type::Int ? static_cast<std::uint64_t>(bits.getSExtValue()) & llvm::APInt::getLowBitsSet(64, numberWidth(info.type)).getZExtValue() : bits.getZExtValue();
        }
        return result;
    }
};

}

void evaluateConstants(CheckedModule& module, const std::vector<Source>& sources) {
    for (const auto& fn : module.functions)
        if (fn.syntax.constant != ConstKind::None) Validator{module, sources, fn}.run();
    module.constants.resize(module.functions.size());
    std::vector<bool> active(module.functions.size());
    for (std::size_t index = 0; index < module.functions.size(); ++index) {
        const auto& fn = module.functions[index];
        if (fn.syntax.constant == ConstKind::Value) Evaluation{module, sources, active}.value(index, fn, fn.syntax.name.span);
    }
}

}
