module;
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <lyra/list_slice.hpp>
#include <lyra/utf8.hpp>
#include <lyra/value_hash.hpp>
#include <map>
#include <set>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module lyra.ctfe;
import lyra.check;
import lyra.numbers;

export namespace lyra {
void evaluateConstants(CheckedModule& module, const std::vector<Source>& sources, const std::function<void(std::size_t)>& requireBody = {});
void evaluateMacro(CheckedModule& module, const std::vector<Source>& sources, MacroExecution& macro, std::size_t entry,
    const std::function<void(std::size_t)>& checkBody);
}

namespace lyra {
namespace {

bool scalar(Type type) {
    return numericType(type) || type == Type::Bool || type == Type::String || type == Type::Bytes ||
        type == Type::Char || type == Type::Unit || type == Type::Never;
}

struct ValueTypes {
    const CheckedModule& module;
    std::map<std::pair<Type, bool>, bool> checked;

    bool supports(Type type, bool temporary = true) {
        if (scalar(type) || type == Type::IntRange) return true;
        auto key = std::pair{type, temporary};
        if (auto prior = checked.find(key); prior != checked.end()) return prior->second;
        std::vector<Type> pending{type};
        std::set<Type> seen;
        while (!pending.empty()) {
            auto part = pending.back();
            pending.pop_back();
            if (scalar(part) || part == Type::IntRange || !seen.insert(part).second) continue;
            if (part.kind == Type::Record && (temporary || !module.aggregates[part.id].syntax.reference)) {
                const auto& fields = module.aggregates[part.id].fields;
                pending.insert(pending.end(), fields.begin(), fields.end());
            } else if (part.kind == Type::Enum) {
                for (const auto& fields : module.enums[part.id].payloads) pending.insert(pending.end(), fields.begin(), fields.end());
            } else if (part.kind == Type::Option || part.kind == Type::Result) {
                const auto& shape = module.sumTypes[part.id];
                pending.push_back(shape.success);
                if (part.kind == Type::Result) pending.push_back(shape.failure);
            } else if (part.kind == Type::FrozenList || (temporary && part.kind == Type::List)) {
                pending.push_back(module.listElements[part.id]);
            } else if (temporary && part.kind == Type::Closure) {
                const auto& shape = module.signatures[part.id];
                pending.insert(pending.end(), shape.parameters.begin(), shape.parameters.end());
                pending.push_back(shape.result);
            } else {
                return checked.emplace(key, false).first->second;
            }
        }
        for (auto part : seen) checked.emplace(std::pair{part, temporary}, true);
        return true;
    }
};

Type rawType(const CheckedFunction& fn, ExprId id) {
    const auto& expr = fn.expressions[id];
    return expr.sourceType.value_or(expr.type);
}

[[noreturn]] void error(const std::vector<Source>& sources, const CheckedFunction& fn, Span site, const char* code, std::string message) {
    throw at(sources[fn.syntax.file], site, code, std::move(message));
}

struct Validation {
    const CheckedModule& module;
    const std::vector<Source>& sources;
    ValueTypes types;
    std::vector<bool> visited;
    std::vector<std::size_t> pending;
    const MacroExecution* macro{};
    std::set<std::size_t> blocks{};
    const std::function<void(std::size_t)>* requireBody{};
};

struct Validator {
    Validation& state;
    const CheckedFunction& fn;

    [[noreturn]] void reject(Span site, std::string detail) const {
        error(state.sources, fn, site, "LYCT0001", std::move(detail));
    }

    void validateFunction(std::size_t index, Span site) const {
        const auto& target = state.module.functions[index];
        const auto& syntax = target.syntax;
        if (state.macro && syntax.file == state.macro->prelude) return;
        if (syntax.pendingExpansion)
            throw at(state.sources[fn.syntax.file], site, "LYM0004", "编译期阶段环：依赖尚未展开或装饰的函数：" + syntax.name.text);
        if (std::ranges::any_of(syntax.parameters, [](const auto& param) { return param.passing != Passing::Value; }))
            reject(site, "编译期不能调用含 in/ref 形参的函数：" + syntax.name.text);
        if (syntax.isAsync || syntax.foreignSymbol || syntax.conversion ||
            (!syntax.parameters.empty() && syntax.parameters.back().variadic))
            reject(site, "编译期不能调用异步、外部、隐式转换或变长函数：" + syntax.name.text);
        if (state.visited.size() < state.module.functions.size()) state.visited.resize(state.module.functions.size());
        if (state.visited[index]) return;
        state.visited[index] = true;
        if (state.requireBody) (*state.requireBody)(index);
        state.pending.push_back(index);
    }

    void validateValueOperation(Type root, ValueOperation operation, Span site) const {
        std::vector<Type> pending{root};
        std::set<Type> visited;
        while (!pending.empty()) {
            auto type = pending.back();
            pending.pop_back();
            if (!visited.insert(type).second) continue;
            if (auto found = state.module.valueOverrides.find({type, operation}); found != state.module.valueOverrides.end()) {
                validateFunction(found->second, site);
                continue;
            }
            auto append = [&](const auto& fields) { pending.insert(pending.end(), fields.begin(), fields.end()); };
            if (type.kind == Type::Record && !state.module.aggregates[type.id].syntax.reference) append(state.module.aggregates[type.id].fields);
            else if (type.kind == Type::Enum) for (const auto& fields : state.module.enums[type.id].payloads) append(fields);
            else if (type.kind == Type::FrozenList) pending.push_back(state.module.listElements[type.id]);
            else if (type.kind == Type::Option || type.kind == Type::Result) {
                const auto& shape = state.module.sumTypes[type.id];
                pending.push_back(shape.success);
                if (type.kind == Type::Result) pending.push_back(shape.failure);
            }
        }
    }

    void validateConstant(ConstantUse target, Span site) const {
        if (!state.macro) return;
        if (target.kind == ConstantUse::Global) validateFunction(target.index, site);
        else if (state.blocks.insert(target.index).second) {
            const auto& block = state.module.blockConstants[target.index];
            const auto& owner = state.module.functions[block.function];
            if (!state.types.supports(block.type, false))
                error(state.sources, owner, block.name.span, "LYCT0001", "块内常量必须保存不可变值；List、Class 和函数值只能暂存");
            Validator{state, owner}.visit(block.initializer);
        }
    }

    void validatePattern(const CheckedPattern& pattern) const {
        if (const auto* constant = std::get_if<PatternConstant>(&pattern.node)) {
            validateValueOperation(constant->type, ValueOperation::Equal, pattern.site);
            validateConstant(constant->target, pattern.site);
        }
        for (const auto& part : pattern.parts) validatePattern(part);
    }

    void visit(ExprId id) const {
        const auto& expr = fn.syntax.expressions[id];
        const auto& info = fn.expressions[id];
        if (info.conversion || info.errorConversion) reject(expr.span, "编译期计算不能调用用户隐式转换");
        if (!state.types.supports(info.type) || !state.types.supports(rawType(fn, id)))
            reject(expr.span, "类型不能用于编译期计算：仅支持标量、Range<Int>、Struct、Class、Enum、Option、Result、List、FrozenList 和同步函数值");
        if (info.functionRef) validateFunction(*info.functionRef, expr.span);
        if (info.constantRef) validateConstant(*info.constantRef, expr.span);
        for (const auto& pattern : info.patterns) validatePattern(pattern);
        const auto descend = [&](ExprId child) { visit(child); };
        std::visit([&](const auto& node) {
            using Node = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<Node, Call>) {
                if (node.function) descend(*node.function);
                if (node.receiver && info.callee != Callee::Variant) descend(*node.receiver);
                for (const auto& arg : node.arguments) descend(arg.value);
                if (node.receiver && fn.expressions[*node.receiver].type == Type::Never) return;
                if (info.callee == Callee::Indirect) {
                    if (info.callableType && !state.types.supports(*info.callableType))
                        reject(node.name.span, "函数值的签名不能用于编译期计算");
                    return;
                }
                if (info.callee == Callee::ToString || info.callee == Callee::ToReal || info.callee == Callee::ToBytes ||
                    info.callee == Callee::ToIntegerChecked || info.callee == Callee::Sum || info.callee == Callee::Variant ||
                    info.callee == Callee::ListAdd || info.callee == Callee::ListSlice || info.callee == Callee::ListRemoveAt ||
                    info.callee == Callee::Size || info.callee == Callee::FreezeList || info.callee == Callee::SumPresent ||
                    info.callee == Callee::SumAbsent || info.callee == Callee::SumUnwrap || info.callee == Callee::CarryFailure ||
                    info.callee == Callee::ListGetOrNone || info.callee == Callee::ListFirstOrNone || info.callee == Callee::ListLastOrNone ||
                    info.callee == Callee::BytesSlice || info.callee == Callee::NumericAbs || info.callee == Callee::NumericMin ||
                    info.callee == Callee::NumericMax || info.callee == Callee::NumericClamp || info.callee == Callee::IsFinite ||
                    info.callee == Callee::IsNaN || info.callee == Callee::IsInfinite) return;
                if (info.callee == Callee::ValueEqual || info.callee == Callee::ValueHash) {
                    auto operation = info.callee == Callee::ValueEqual ? ValueOperation::Equal : ValueOperation::Hash;
                    validateValueOperation(fn.expressions[*node.receiver].type, operation, node.name.span);
                    return;
                }
                if (info.callee == Callee::Constructor) {
                    const auto& record = state.module.aggregates[info.target];
                    for (auto slot : info.omittedDefaults)
                        if (record.defaultFunctions[slot]) validateFunction(*record.defaultFunctions[slot], node.name.span);
                    return;
                }
                if (info.callee == Callee::Function) {
                    validateFunction(info.target, node.name.span);
                    const auto& target = state.module.functions[info.target];
                    for (auto slot : info.omittedDefaults) validateFunction(*target.defaultFunctions[slot], node.name.span);
                    return;
                }
                reject(node.name.span, "编译期只能调用纯函数和内建值操作：" + node.name.text);
            } else if constexpr (std::is_same_v<Node, Binary>) {
                descend(node.left);
                descend(node.right);
                if (node.op == Op::Eq || node.op == Op::Ne) validateValueOperation(fn.expressions[node.left].type, ValueOperation::Equal, node.site);
            } else if constexpr (std::is_same_v<Node, RangeValue>) {
                descend(node.start);
                descend(node.end);
            } else if constexpr (std::is_same_v<Node, DisplayValue>) {
                descend(node.value);
                if (fn.expressions[node.value].type.kind == Type::Record) validateFunction(info.target, expr.span);
            } else if constexpr (std::is_same_v<Node, Binding>) {
                if (node.constant) validateConstant({ConstantUse::Block, info.target}, node.name.span);
                else descend(node.value);
            }
            else if constexpr (std::is_same_v<Node, PatternBinding>) descend(node.value);
            else if constexpr (std::is_same_v<Node, Lambda>) {
                if (node.isAsync) reject(expr.span, "编译期计算不支持异步 lambda");
                descend(node.body);
            }
            else if constexpr (std::is_same_v<Node, Unary>) descend(node.operand);
            else if constexpr (std::is_same_v<Node, Member>) {
                if (info.callee != Callee::Variant) descend(node.receiver);
                if (info.callee == Callee::ExtensionProperty) validateFunction(info.target, expr.span);
            }
            else if constexpr (std::is_same_v<Node, Index>) { descend(node.receiver); descend(node.offset); }
            else if constexpr (std::is_same_v<Node, ListLiteral>) { for (auto element : node.elements) descend(element); }
            else if constexpr (std::is_same_v<Node, Propagate>) descend(node.operand);
            else if constexpr (std::is_same_v<Node, TextTemplate>) { for (auto part : node.parts) descend(part); }
            else if constexpr (std::is_same_v<Node, Block>) { for (const auto& statement : node.statements) descend(statement.expression); }
            else if constexpr (std::is_same_v<Node, Conditional>) {
                descend(node.condition);
                descend(node.yes);
                if (node.no) descend(*node.no);
            } else if constexpr (std::is_same_v<Node, While>) {
                descend(node.condition);
                if (fn.expressions[node.condition].type != Type::Never) descend(node.body);
            } else if constexpr (std::is_same_v<Node, For>) {
                if (node.awaitSite) reject(*node.awaitSite, "编译期不能异步遍历");
                descend(node.input);
                if (info.iteration && info.iteration->methods) {
                    validateFunction(info.iteration->methods->open, expr.span);
                    validateFunction(info.iteration->methods->next, expr.span);
                }
                if (info.iteration) descend(node.body);
            } else if constexpr (std::is_same_v<Node, Match>) {
                descend(node.subject);
                for (const auto& arm : node.arms) {
                    if (arm.guard) descend(*arm.guard);
                    descend(arm.value);
                }
            } else if constexpr (std::is_same_v<Node, Return>) { if (node.value) descend(*node.value); }
            else if constexpr (!(std::is_same_v<Node, Identifier> || std::is_same_v<Node, This> || std::is_same_v<Node, QualifiedMember> ||
                std::is_same_v<Node, EmptyOption> || std::is_same_v<Node, Jump> || std::is_same_v<Node, bool> ||
                std::is_same_v<Node, IntegerLiteral> || std::is_same_v<Node, RealLiteral> || std::is_same_v<Node, CharLiteral> ||
                std::is_same_v<Node, StringLiteral> || std::is_same_v<Node, BytesLiteral>)) reject(expr.span, "编译期计算不支持此表达式");
        }, expr.node);
    }

    void run() const {
        if (fn.expressions.empty() && !fn.syntax.expressions.empty()) throw EditorBodyRequired{};
        if (fn.syntax.constant == ConstKind::Value && !state.types.supports(fn.result, false))
            reject(fn.syntax.result.span, "编译期常量必须是不可变值类型；函数值、Class 和 List 只能作为编译期临时值");
        if (!state.types.supports(fn.result)) reject(fn.syntax.result.span, "编译期函数返回类型不受支持");
        for (std::size_t slot = 0; slot < fn.syntax.parameters.size(); ++slot)
            if (!state.types.supports(fn.locals[slot].type)) reject(fn.syntax.parameters[slot].type.span, "函数形参类型不能用于编译期计算");
        visit(fn.syntax.body);
    }
};

using EvalValue = ConstValue;
struct Returned { EvalValue value; };
using EvalSlot = std::variant<EvalValue, EvalValue*>;
using PatternBindings = std::vector<std::pair<std::size_t, EvalValue>>;

struct Frame {
    std::vector<EvalSlot> slots;
    Type result;

    EvalValue& cell(std::size_t slot) {
        if (auto shared = std::get_if<EvalValue*>(&slots[slot])) return **shared;
        return std::get<EvalValue>(slots[slot]);
    }
};

struct Closure {
    const CheckedFunction& function;
    std::optional<ExprId> lambda{};
    std::vector<EvalValue*> captures{};
    std::optional<EvalValue> receiver{};
};

struct Evaluation {
    CheckedModule& module;
    const std::vector<Source>& sources;
    std::vector<bool>& active;
    unsigned remaining = 100000, depth = 0;
    std::size_t bytes = 0;
    std::deque<ConstComposite> composites{};
    std::deque<EvalValue> cells{};
    std::map<std::size_t, Closure> closures{};
    std::map<std::size_t, ConstRef> saved{}, loaded{};
    const MacroExecution* macro{};
    Type syntaxType{};

    void step(const CheckedFunction& fn, Span site) {
        checkAnalysisCancellation();
        if (!remaining--) error(sources, fn, site, "LYCT0004", "编译期计算超出执行步数预算");
    }

    const ConstScalar& scalarValue(const EvalValue& value, const CheckedFunction& fn, Span site) const {
        if (const auto* scalar = std::get_if<ConstScalar>(&value)) return *scalar;
        error(sources, fn, site, "LYCT0001", "此编译期操作需要标量值");
    }

    ConstComposite& composite(const EvalValue& value) {
        return composites[std::get<ConstRef>(value).index];
    }

    EvalValue makeComposite(const CheckedFunction& fn, Span site, Type type, ConstTag tag, std::vector<EvalValue> fields = {}) {
        charge(fn, site, sizeof(ConstComposite) + fields.size() * sizeof(EvalValue));
        auto index = composites.size();
        composites.push_back({type, std::move(tag), std::move(fields)});
        return ConstRef{index};
    }

    EvalValue makeClosure(const CheckedFunction& fn, Span site, Type type, Closure closure) {
        charge(fn, site, sizeof(Closure) + closure.captures.size() * sizeof(EvalValue*));
        auto value = makeComposite(fn, site, type, std::monostate{});
        closures.emplace(std::get<ConstRef>(value).index, std::move(closure));
        return value;
    }

    void initialize(const CheckedFunction& fn, Span site, Frame& frame, std::size_t slot, EvalValue value) {
        if (!fn.locals[slot].captured) frame.slots[slot] = std::move(value);
        else {
            charge(fn, site, sizeof(EvalValue));
            cells.push_back(std::move(value));
            frame.slots[slot] = &cells.back();
        }
    }

    void valueStep(const CheckedFunction& fn, Span site, unsigned level) {
        step(fn, site);
        if (level > 128) error(sources, fn, site, "LYCT0004", "编译期结构相等或哈希的嵌套深度超出预算（128）");
    }

    bool equal(const EvalValue& left, const EvalValue& right, Type type, const CheckedFunction& caller, Span site, unsigned level = 0) {
        valueStep(caller, site, level);
        if (auto override = module.valueOverrides.find({type, ValueOperation::Equal}); override != module.valueOverrides.end()) {
            auto result = invoke(module.functions[override->second], {left, right}, caller, site);
            return std::get<bool>(scalarValue(result, caller, site));
        }
        if (type.kind == Type::Record || type == Type::IntRange) {
            if (type.kind == Type::Record && module.aggregates[type.id].syntax.reference) return left == right;
            auto fields = type == Type::IntRange ? std::span<const Type>(rangeFields) : std::span<const Type>(module.aggregates[type.id].fields);
            const auto& first = composite(left).fields;
            const auto& second = composite(right).fields;
            for (std::size_t slot = 0; slot < fields.size(); ++slot)
                if (!equal(first[slot], second[slot], fields[slot], caller, site, level + 1)) return false;
            return true;
        }
        if (type.kind == Type::FrozenList) {
            const auto& first = composite(left).fields;
            const auto& second = composite(right).fields;
            auto element = module.listElements[type.id];
            return std::ranges::equal(first, second, [&](const auto& lhs, const auto& rhs) { return equal(lhs, rhs, element, caller, site, level + 1); });
        }
        if (type.kind == Type::Enum) {
            const auto& first = composite(left);
            const auto& second = composite(right);
            if (first.tag != second.tag) return false;
            auto tag = std::get<VariantTag>(first.tag);
            const auto& fields = module.enums[type.id].payloads[tag.index];
            for (std::size_t slot = 0; slot < fields.size(); ++slot)
                if (!equal(first.fields[slot], second.fields[slot], fields[slot], caller, site, level + 1)) return false;
            return true;
        }
        if (type.kind == Type::Option || type.kind == Type::Result) {
            const auto& first = composite(left);
            const auto& second = composite(right);
            if (first.tag != second.tag) return false;
            auto tag = std::get<SumTag>(first.tag);
            if (tag == SumTag::None) return true;
            const auto& shape = module.sumTypes[type.id];
            auto field = successTag(tag) ? shape.success : shape.failure;
            return equal(first.fields.front(), second.fields.front(), field, caller, site, level + 1);
        }
        return left == right;
    }

    std::uint64_t hash(const EvalValue& value, Type type, const CheckedFunction& caller, Span site, unsigned level = 0) {
        valueStep(caller, site, level);
        if (auto override = module.valueOverrides.find({type, ValueOperation::Hash}); override != module.valueOverrides.end()) {
            auto result = invoke(module.functions[override->second], {value}, caller, site);
            return integer(Type::Int, scalarValue(result, caller, site)).getZExtValue();
        }
        auto combine = [](std::uint64_t state, std::uint64_t part) { return mixValueHash(state, part); };
        if (type == Type::String || type == Type::Bytes)
            return hashValueBytes(std::get<std::string>(scalarValue(value, caller, site)));
        if (type.kind == Type::Int || type.kind == Type::UInt)
            return combine(valueHashSeed, integer(type, scalarValue(value, caller, site)).getZExtValue());
        if (type.kind == Type::Real) {
            auto number = std::get<double>(scalarValue(value, caller, site));
            std::uint64_t bits;
            if (numberWidth(type) == 32) {
                auto narrowed = static_cast<float>(number);
                bits = narrowed == 0.0f ? 0 : std::bit_cast<std::uint32_t>(narrowed);
            } else bits = number == 0.0 ? 0 : std::bit_cast<std::uint64_t>(number);
            return combine(valueHashSeed, bits);
        }
        if (type.kind == Type::Bool) return combine(valueHashSeed, std::get<bool>(scalarValue(value, caller, site)));
        if (type.kind == Type::Char) return combine(valueHashSeed, std::get<std::uint64_t>(scalarValue(value, caller, site)));
        if (type == Type::Unit) return combine(valueHashSeed, 0);
        if (type.kind == Type::FrozenList) {
            const auto& fields = composite(value).fields;
            auto result = combine(valueHashSeed, fields.size());
            for (const auto& item : fields) result = combine(result, hash(item, module.listElements[type.id], caller, site, level + 1));
            return result;
        }
        if (type.kind == Type::Record || type == Type::IntRange) {
            if (type.kind == Type::Record && module.aggregates[type.id].syntax.reference) error(sources, caller, site, "LYCT0001", "Class 按身份比较，但没有默认 Hash");
            auto result = valueHashSeed;
            auto fields = type == Type::IntRange ? std::span<const Type>(rangeFields) : std::span<const Type>(module.aggregates[type.id].fields);
            for (std::size_t slot = 0; slot < fields.size(); ++slot)
                result = combine(result, hash(composite(value).fields[slot], fields[slot], caller, site, level + 1));
            return result;
        }
        if (type.kind == Type::Enum) {
            const auto& item = composite(value);
            auto tag = std::get<VariantTag>(item.tag);
            auto result = combine(valueHashSeed, tag.index);
            const auto& fields = module.enums[type.id].payloads[tag.index];
            for (std::size_t slot = 0; slot < fields.size(); ++slot) result = combine(result, hash(item.fields[slot], fields[slot], caller, site, level + 1));
            return result;
        }
        if (type.kind == Type::Option || type.kind == Type::Result) {
            const auto& item = composite(value);
            auto tag = std::get<SumTag>(item.tag);
            auto result = combine(valueHashSeed, static_cast<std::uint64_t>(successTag(tag)));
            if (tag != SumTag::None) {
                const auto& shape = module.sumTypes[type.id];
                auto field = successTag(tag) ? shape.success : shape.failure;
                result = combine(result, hash(item.fields.front(), field, caller, site, level + 1));
            }
            return result;
        }
        error(sources, caller, site, "LYCT0001", "类型不能用于编译期哈希");
    }

    bool matches(const CheckedFunction& fn, Span site, const CheckedPattern& pattern, const EvalValue& subject, Type type,
                 PatternBindings& bindings, std::optional<std::size_t> listOffset = {}) {
        step(fn, site);
        if (std::holds_alternative<std::monostate>(pattern.node)) return true;
        if (const auto* slot = std::get_if<PatternSlot>(&pattern.node)) {
            bindings.emplace_back(slot->local, subject);
            return true;
        }
        if (const auto* rest = std::get_if<PatternListRest>(&pattern.node)) {
            if (type.kind != Type::List) return false;
            const auto& values = composite(subject).fields;
            auto start = listOffset.value_or(0);
            bindings.emplace_back(rest->local, makeComposite(fn, site, type, std::monostate{},
                std::vector<EvalValue>(values.begin() + start, values.end())));
            return true;
        }
        if (const auto* constant = std::get_if<PatternConstant>(&pattern.node))
            return equal(subject, value(constant->target, fn, site), constant->type, fn, site);
        if (const auto* constructor = std::get_if<PatternConstructor>(&pattern.node)) {
            if (const auto* sum = std::get_if<SumTag>(constructor)) {
                if (type.kind != Type::Option && type.kind != Type::Result) return false;
                const auto& value = composite(subject);
                if (std::get<SumTag>(value.tag) != *sum) return false;
                if (*sum == SumTag::None) return true;
                const auto& fields = module.sumTypes[type.id];
                auto payloadType = successTag(*sum) ? fields.success : fields.failure;
                return pattern.parts.empty() || matches(fn, site, pattern.parts.front(), value.fields.front(), payloadType, bindings);
            }
            if (const auto* variant = std::get_if<VariantTag>(constructor)) {
                if (type.kind != Type::Enum) return false;
                const auto& value = composite(subject);
                if (std::get<VariantTag>(value.tag) != *variant) return false;
                const auto& fields = module.enums[type.id].payloads[variant->index];
                for (std::size_t index = 0; index < pattern.parts.size(); ++index)
                    if (!matches(fn, site, pattern.parts[index], value.fields[index], fields[index], bindings)) return false;
                return true;
            }
            if (const auto* list = std::get_if<ListTag>(constructor)) {
                if (type.kind != Type::List) return false;
                const auto& values = composite(subject).fields;
                auto offset = listOffset.value_or(0);
                if (offset > values.size()) return false;
                if (!list->cons) return offset == values.size();
                if (offset == values.size()) return false;
                return matches(fn, site, pattern.parts[0], values[offset], module.listElements[type.id], bindings) &&
                    matches(fn, site, pattern.parts[1], subject, type, bindings, offset + 1);
            }
            if (std::holds_alternative<PatternProduct>(*constructor)) {
                if (type.kind != Type::Record) return false;
                const auto& value = composite(subject);
                const auto& fields = module.aggregates[type.id].fields;
                for (std::size_t index = 0; index < pattern.parts.size(); ++index)
                    if (!matches(fn, site, pattern.parts[index], value.fields[index], fields[index], bindings)) return false;
                return true;
            }
            if (std::holds_alternative<PatternClass>(*constructor)) {
                if (type.kind != Type::Record || !module.aggregates[type.id].syntax.reference) return false;
                const auto& value = composite(subject);
                const auto& fields = module.aggregates[type.id].fields;
                for (std::size_t index = 0; index < pattern.parts.size(); ++index)
                    if (!matches(fn, site, pattern.parts[index], value.fields[index], fields[index], bindings)) return false;
                return true;
            }
            return std::get<bool>(*constructor) == std::get<bool>(scalarValue(subject, fn, site));
        }
        if (const auto* literal = std::get_if<std::uint64_t>(&pattern.node)) {
            const auto& value = scalarValue(subject, fn, site);
            if (type == Type::Char) return std::get<std::uint64_t>(value) == *literal;
            return integer(type, value) == llvm::APInt(numberWidth(type), *literal);
        }
        if (const auto* text = std::get_if<StringLiteral>(&pattern.node)) return std::get<std::string>(scalarValue(subject, fn, site)) == text->bytes;
        if (const auto* range = std::get_if<CheckedIntegerRange>(&pattern.node)) {
            const auto value = integer(type, scalarValue(subject, fn, site));
            const llvm::APInt lower(numberWidth(type), range->lower), upper(numberWidth(type), range->upper);
            if (range->signedValues) return value.sge(lower) && (range->inclusive ? value.sle(upper) : value.slt(upper));
            return value.uge(lower) && (range->inclusive ? value.ule(upper) : value.ult(upper));
        }
        if (std::holds_alternative<CheckedAlternatives>(pattern.node)) {
            auto checkpoint = bindings.size();
            for (const auto& part : pattern.parts) {
                if (matches(fn, site, part, subject, type, bindings, listOffset)) return true;
                bindings.resize(checkpoint);
            }
            return false;
        }
        std::abort();
    }

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
        if (size > 32 * 1024 * 1024 - bytes) error(sources, fn, site, "LYCT0004", "编译期值超出内存预算");
        bytes += size;
    }

    void chargeText(const CheckedFunction& fn, Span site, std::size_t size) {
        if (size > 1024 * 1024) error(sources, fn, site, "LYCT0004", "编译期文本超出字节预算");
        charge(fn, site, size);
    }

    ConstValue transfer(const ConstValue& value, const std::deque<ConstComposite>& from, std::deque<ConstComposite>& to,
                        std::map<std::size_t, ConstRef>& memo, const CheckedFunction& fn, Span site, unsigned nesting = 0) {
        if (const auto* scalar = std::get_if<ConstScalar>(&value)) {
            if (const auto* text = std::get_if<std::string>(scalar)) chargeText(fn, site, text->size());
            return value;
        }
        auto index = std::get<ConstRef>(value).index;
        if (auto found = memo.find(index); found != memo.end()) {
            if (nesting + to[found->second.index].height > 128)
                error(sources, fn, site, "LYCT0004", "编译期常量的值嵌套超出预算（128）");
            return found->second;
        }
        if (nesting >= 128) error(sources, fn, site, "LYCT0004", "编译期常量的值嵌套超出预算（128）");
        const auto& node = from[index];
        charge(fn, site, sizeof(ConstComposite) + node.fields.size() * sizeof(ConstValue));
        std::vector<ConstValue> fields;
        fields.reserve(node.fields.size());
        unsigned height = 1;
        for (const auto& field : node.fields) {
            auto copied = transfer(field, from, to, memo, fn, site, nesting + 1);
            if (auto ref = std::get_if<ConstRef>(&copied)) height = std::max(height, to[ref->index].height + 1);
            fields.push_back(std::move(copied));
        }
        auto stored = ConstRef{to.size()};
        to.push_back({node.type, node.tag, std::move(fields), height});
        memo.emplace(index, stored);
        return stored;
    }

    EvalValue invoke(const CheckedFunction& fn, std::vector<EvalValue> args, const CheckedFunction& caller, Span call, const Closure* closure = nullptr, const BlockConstant* block = nullptr) {
        if (fn.expressions.empty() && !fn.syntax.expressions.empty()) throw EditorBodyRequired{};
        if (macro && fn.syntax.file == macro->prelude) {
            auto name = std::string_view(fn.syntax.name.text);
            if (auto separator = name.rfind("::"); separator != name.npos) name.remove_prefix(separator + 2);
            std::vector<MacroValue> values;
            for (std::size_t slot = 0; slot < args.size(); ++slot) {
                auto type = fn.locals[slot].type;
                if (type == syntaxType) values.emplace_back(SyntaxRef{std::get<std::uint64_t>(scalarValue(composite(args[slot]).fields.front(), caller, call))});
                else {
                    const auto& value = scalarValue(args[slot], caller, call);
                    if (type == Type::Int) values.emplace_back(std::bit_cast<std::int64_t>(std::get<std::uint64_t>(value)));
                    else if (type == Type::Bool) values.emplace_back(std::get<bool>(value));
                    else values.emplace_back(std::get<std::string>(value));
                }
            }
            auto result = macro->host(name, values);
            auto syntax = [&](SyntaxRef ref) {
                return makeComposite(caller, call, syntaxType, std::monostate{}, {ConstScalar{static_cast<std::uint64_t>(ref.index)}});
            };
            if (fn.result == syntaxType) return syntax(std::get<SyntaxRef>(result));
            if (fn.result.kind == Type::Option && module.sumTypes[fn.result.id].success == syntaxType) {
                if (auto ref = std::get_if<SyntaxRef>(&result)) return makeComposite(caller, call, fn.result, SumTag::Some, {syntax(*ref)});
                return makeComposite(caller, call, fn.result, SumTag::None);
            }
            if (fn.result == Type::Int) return std::bit_cast<std::uint64_t>(std::get<std::int64_t>(result));
            if (fn.result == Type::Bool) return std::get<bool>(result);
            if (fn.result == Type::String) return std::get<std::string>(std::move(result));
            return {};
        }
        if (++depth > (macro ? 64U : 16U)) error(sources, caller, call, "LYCT0004", "编译期调用深度超出预算（" + std::to_string(macro ? 64 : 16) + "）");
        const auto frameBytes = fn.locals.size() * sizeof(EvalSlot);
        charge(caller, call, frameBytes);
        Frame frame{std::vector<EvalSlot>(fn.locals.size()), fn.result};
        auto body = fn.syntax.body;
        const auto receiver = fn.syntax.parameters.size();
        const auto initializer = !closure && !block && fn.syntax.initializer;
        if (block) {
            body = block->initializer;
            frame.result = block->type;
        } else if (closure) {
            auto lambda = *closure->lambda;
            const auto& info = fn.expressions[lambda];
            for (std::size_t index = 0; index < info.captures.size(); ++index) frame.slots[info.captures[index]] = closure->captures[index];
            for (std::size_t index = 0; index < args.size(); ++index)
                initialize(fn, fn.syntax.expressions[lambda].span, frame, info.argumentSlots[index], std::move(args[index]));
            frame.result = module.signatures[rawType(fn, lambda).id].result;
            body = std::get<Lambda>(fn.syntax.expressions[lambda].node).body;
        } else {
            for (std::size_t slot = 0; slot < args.size(); ++slot) initialize(fn, fn.syntax.name.span, frame, slot, std::move(args[slot]));
            if (initializer) {
                const auto fields = module.aggregates[fn.result.id].fields.size();
                auto object = makeComposite(caller, call, fn.result, std::monostate{}, std::vector<EvalValue>(fields));
                initialize(fn, fn.syntax.name.span, frame, receiver, std::move(object));
            }
        }
        EvalValue result;
        try { result = eval(fn, body, frame); }
        catch (Returned& returned) { result = std::move(returned.value); }
        catch (...) { bytes -= frameBytes; --depth; throw; }
        bytes -= frameBytes;
        --depth;
        return initializer ? frame.cell(receiver) : std::move(result);
    }

    EvalValue value(ConstantUse target, const CheckedFunction& caller, Span site) {
        auto block = target.kind == ConstantUse::Block ? &module.blockConstants[target.index] : nullptr;
        auto& cached = block ? block->value : module.constants[target.index];
        if (cached) return transfer(*cached, module.constantComposites, composites, loaded, caller, site);
        const auto& fn = module.functions[block ? block->function : target.index];
        auto index = block ? module.functions.size() + target.index : target.index;
        if (active[index]) error(sources, caller, site, "LYCT0002", "编译期常量存在依赖环：" + (block ? block->name.text : fn.syntax.name.text));
        active[index] = true;
        auto evaluated = invoke(fn, {}, caller, site, nullptr, block);
        cached = transfer(evaluated, composites, module.constantComposites, saved, caller, site);
        if (auto stored = std::get_if<ConstRef>(&*cached)) {
            auto local = std::get<ConstRef>(evaluated);
            composites[local.index].height = module.constantComposites[stored->index].height;
            loaded.emplace(stored->index, local);
        }
        active[index] = false;
        return evaluated;
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
            default: std::abort();
        }
    }

    EvalValue operation(const CheckedFunction& fn, Span site, Op op, Type type, const EvalValue& leftValue, const EvalValue& rightValue) {
        if (op == Op::Eq || op == Op::Ne) return equal(leftValue, rightValue, type, fn, site) == (op == Op::Eq);
        const auto& left = scalarValue(leftValue, fn, site);
        const auto& right = scalarValue(rightValue, fn, site);
        if ((type == Type::String || type == Type::Bytes) && op == Op::Add) return std::string(std::get<std::string>(left) + std::get<std::string>(right));
        if (!numericType(type)) error(sources, fn, site, "LYCT0001", "此编译期运算不支持此类型");
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
                default: std::abort();
            }
            return asDouble(first, type);
        }
        auto first = integer(type, left);
        if (shift(op)) {
            auto count = std::get<std::uint64_t>(right);
            if (count >= numberWidth(type)) error(sources, fn, site, "LYCT0003", "编译期位移量超出范围");
            auto offset = static_cast<unsigned>(count);
            return (op == Op::Shl ? first.shl(offset) : type.kind == Type::Int ? first.ashr(offset) : first.lshr(offset)).getZExtValue();
        }
        auto second = integer(type, right);
        bool sign = type.kind == Type::Int, overflow = false;
        llvm::APInt result;
        switch (op) {
            case Op::Eq: return first == second;
            case Op::Ne: return first != second;
            case Op::Lt: return sign ? first.slt(second) : first.ult(second);
            case Op::Le: return sign ? first.sle(second) : first.ule(second);
            case Op::Gt: return sign ? first.sgt(second) : first.ugt(second);
            case Op::Ge: return sign ? first.sge(second) : first.uge(second);
            case Op::BitAnd: return (first & second).getZExtValue();
            case Op::BitXor: return (first ^ second).getZExtValue();
            case Op::BitOr: return (first | second).getZExtValue();
            case Op::Add: result = sign ? first.sadd_ov(second, overflow) : first.uadd_ov(second, overflow); break;
            case Op::Sub: result = sign ? first.ssub_ov(second, overflow) : first.usub_ov(second, overflow); break;
            case Op::Mul: result = sign ? first.smul_ov(second, overflow) : first.umul_ov(second, overflow); break;
            case Op::Div: case Op::Rem:
                if (second.isZero()) error(sources, fn, site, "LYCT0003", "编译期整数除零");
                if (op == Op::Div) result = sign ? first.sdiv_ov(second, overflow) : first.udiv(second);
                else if (sign && second.isAllOnes()) return std::uint64_t{0};
                else result = sign ? first.srem(second) : first.urem(second);
                break;
            default: std::abort();
        }
        if (overflow) error(sources, fn, site, "LYCT0003", "编译期整数运算超出 " + numberName(type) + " 的范围");
        return result.getZExtValue();
    }

    EvalValue numericMember(const CheckedFunction& fn, Span site, Type type, Callee operation, const ConstScalar& input,
                            const ConstScalar& first, const ConstScalar& second) const {
        if (type.kind == Type::Real) {
            auto value = real(type, std::get<double>(input));
            if (operation == Callee::IsFinite) return value.isFinite();
            if (operation == Callee::IsNaN) return value.isNaN();
            if (operation == Callee::IsInfinite) return value.isInfinity();
            if (operation == Callee::NumericAbs) return asDouble(llvm::abs(value), type);
            auto other = real(type, std::get<double>(first));
            if (operation == Callee::NumericMin) return asDouble(llvm::minimum(value, other), type);
            if (operation == Callee::NumericMax) return asDouble(llvm::maximum(value, other), type);
            auto upper = real(type, std::get<double>(second));
            auto order = other.compare(upper);
            if (order == llvm::APFloat::cmpGreaterThan || order == llvm::APFloat::cmpUnordered)
                error(sources, fn, site, "LYCT0003", "invalid clamp bounds");
            return asDouble(llvm::minimum(llvm::maximum(value, other), upper), type);
        }
        auto value = integer(type, input);
        bool signedValue = type.kind == Type::Int;
        if (operation == Callee::NumericAbs) {
            if (signedValue && value.isMinSignedValue()) error(sources, fn, site, "LYCT0003", numberName(type) + " overflow (abs)");
            return signedValue && value.isNegative() ? (-value).getZExtValue() : value.getZExtValue();
        }
        auto less = [&](const llvm::APInt& left, const llvm::APInt& right) { return signedValue ? left.slt(right) : left.ult(right); };
        auto other = integer(type, first);
        if (operation == Callee::NumericMin) return (less(value, other) ? value : other).getZExtValue();
        if (operation == Callee::NumericMax) return (less(value, other) ? other : value).getZExtValue();
        auto upper = integer(type, second);
        if (less(upper, other)) error(sources, fn, site, "LYCT0003", "invalid clamp bounds");
        return (less(value, other) ? other : less(upper, value) ? upper : value).getZExtValue();
    }

    std::optional<std::uint64_t> convertInteger(Type from, Type option, const ConstScalar& value) const {
        auto target = module.sumTypes[option.id].success;
        auto width = numberWidth(target);
        if (integerType(from)) {
            auto input = integer(from, value);
            auto wideWidth = std::max(numberWidth(from), width) + 1;
            auto widened = from.kind == Type::Int ? input.sextOrTrunc(wideWidth) : input.zextOrTrunc(wideWidth);
            auto minimum = target.kind == Type::Int ? llvm::APInt::getSignedMinValue(width).sext(wideWidth) : llvm::APInt(wideWidth, 0);
            auto maximum = target.kind == Type::Int ? llvm::APInt::getSignedMaxValue(width).sext(wideWidth) : llvm::APInt::getMaxValue(width).zext(wideWidth);
            if (widened.slt(minimum) || widened.sgt(maximum)) return {};
            return widened.trunc(width).getZExtValue();
        }
        auto input = std::get<double>(value);
        auto limit = std::ldexp(1.0, static_cast<int>(width) - (target.kind == Type::Int));
        auto minimum = target.kind == Type::Int ? -limit : 0.0;
        if (!std::isfinite(input) || input < minimum || input >= limit || std::trunc(input) != input) return {};
        return target.kind == Type::Int ? static_cast<std::uint64_t>(static_cast<std::int64_t>(input)) : static_cast<std::uint64_t>(input);
    }

    std::size_t listIndex(const EvalValue& list, const EvalValue& index, const CheckedFunction& fn, Span site) {
        auto offset = integer(Type::Int, scalarValue(index, fn, site)).getSExtValue();
        if (offset < 0 || static_cast<std::uint64_t>(offset) >= composite(list).fields.size())
            error(sources, fn, site, "LYCT0003", "编译期索引越界");
        return static_cast<std::size_t>(offset);
    }

    struct RangeParts { std::int64_t start, end; bool inclusive; };

    RangeParts rangeParts(const EvalValue& range, const CheckedFunction& fn, Span site) {
        const auto& fields = composite(range).fields;
        return {integer(Type::Int, scalarValue(fields[0], fn, site)).getSExtValue(), integer(Type::Int, scalarValue(fields[1], fn, site)).getSExtValue(),
                std::get<bool>(scalarValue(fields[2], fn, site))};
    }

    struct Place { EvalValue* local; std::optional<ConstRef> owner; std::size_t index; std::vector<std::size_t> path{}; };

    Place address(const CheckedFunction& fn, ExprId id, Frame& frame) {
        const auto& node = fn.syntax.expressions[id].node;
        if (std::holds_alternative<Identifier>(node) || std::holds_alternative<This>(node))
            return {&frame.cell(fn.expressions[id].target), {}, 0};
        if (const auto* index = std::get_if<Index>(&node)) {
            auto owner = eval(fn, index->receiver, frame);
            auto offset = eval(fn, index->offset, frame);
            return {nullptr, std::get<ConstRef>(owner), listIndex(owner, offset, fn, index->site)};
        }
        auto receiver = std::get<Member>(node).receiver;
        auto type = fn.expressions[receiver].type;
        auto slot = fn.expressions[id].target;
        if (type.kind == Type::Record && module.aggregates[type.id].syntax.reference) {
            auto owner = eval(fn, receiver, frame);
            return {nullptr, std::get<ConstRef>(owner), slot};
        }
        auto place = address(fn, receiver, frame);
        place.path.push_back(slot);
        return place;
    }

    EvalValue& cell(const Place& place, const CheckedFunction& fn, Span site) {
        if (!place.owner) return *place.local;
        auto& fields = composites[place.owner->index].fields;
        if (place.index >= fields.size()) error(sources, fn, site, "LYCT0003", "编译期索引越界");
        return fields[place.index];
    }

    EvalValue replace(const CheckedFunction& fn, Span site, const EvalValue& base,
                      const std::vector<std::size_t>& path, std::size_t depth, EvalValue value) {
        if (depth == path.size()) return value;
        auto& node = composite(base);
        auto fields = node.fields;
        auto slot = path[depth];
        fields[slot] = replace(fn, site, fields[slot], path, depth + 1, std::move(value));
        return makeComposite(fn, site, node.type, node.tag, std::move(fields));
    }

    EvalValue eval(const CheckedFunction& fn, ExprId id, Frame& frame) {
        const auto& info = fn.expressions[id];
        const auto& [site, node] = fn.syntax.expressions[id];
        step(fn, site);
        auto sub = [&](ExprId child) { return eval(fn, child, frame); };
        auto result = std::visit([&](const auto& item) -> EvalValue {
            using Node = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Node, IntegerLiteral>) {
                if (info.realLiteral) return *info.realLiteral;
                return llvm::APInt(numberWidth(rawType(fn, id)), integerBits(item)).getZExtValue();
            } else if constexpr (std::is_same_v<Node, RealLiteral>) return *info.realLiteral;
            else if constexpr (std::is_same_v<Node, bool>) return item;
            else if constexpr (std::is_same_v<Node, CharLiteral>) return static_cast<std::uint64_t>(item.point);
            else if constexpr (std::is_same_v<Node, StringLiteral> || std::is_same_v<Node, BytesLiteral>) return item.bytes;
            else if constexpr (std::is_same_v<Node, Identifier>) {
                if (info.constantRef) return value(*info.constantRef, fn, site);
                if (info.functionRef) return makeClosure(fn, site, rawType(fn, id), Closure{module.functions[*info.functionRef]});
                auto& local = frame.cell(info.target);
                return info.narrowed ? composite(local).fields.front() : local;
            }
            else if constexpr (std::is_same_v<Node, This>) return frame.cell(info.target);
            else if constexpr (std::is_same_v<Node, Binding>) { if (!item.constant) initialize(fn, site, frame, info.target, sub(item.value)); }
            else if constexpr (std::is_same_v<Node, PatternBinding>) {
                auto value = sub(item.value);
                PatternBindings bindings;
                if (!matches(fn, item.pattern.span, info.patterns.front(), value, fn.expressions[item.value].type, bindings))
                    error(sources, fn, item.pattern.span, "LYT0012", "val 解构必须匹配所有可能的值");
                for (auto& [slot, bound] : bindings) initialize(fn, site, frame, slot, std::move(bound));
            }
            else if constexpr (std::is_same_v<Node, Lambda>) {
                Closure closure{fn, id};
                closure.captures.reserve(info.captures.size());
                for (auto slot : info.captures) closure.captures.push_back(std::get<EvalValue*>(frame.slots[slot]));
                return makeClosure(fn, site, rawType(fn, id), std::move(closure));
            }
            else if constexpr (std::is_same_v<Node, DisplayValue>) {
                auto value = sub(item.value);
                auto type = fn.expressions[item.value].type;
                if (type.kind == Type::Record) return invoke(module.functions[info.target], {value}, fn, site);
                return display(scalarValue(value, fn, site), type);
            }
            else if constexpr (std::is_same_v<Node, TextTemplate>) {
                std::string combined;
                for (auto part : item.parts) {
                    combined += std::get<std::string>(scalarValue(sub(part), fn, site));
                    if (combined.size() > 1024 * 1024) chargeText(fn, site, combined.size());
                }
                return combined;
            } else if constexpr (std::is_same_v<Node, Member>) {
                if (info.callee == Callee::Variant)
                    return makeComposite(fn, site, rawType(fn, id), VariantTag{rawType(fn, id).id, static_cast<unsigned>(info.target)});
                auto receiver = sub(item.receiver);
                if (info.callee == Callee::BoundMethod)
                    return makeClosure(fn, site, rawType(fn, id), Closure{module.functions[*info.functionRef], {}, {}, std::move(receiver)});
                if (info.callee == Callee::ExtensionProperty) return invoke(module.functions[info.target], {std::move(receiver)}, fn, site);
                auto owner = fn.expressions[item.receiver].type;
                if (owner.kind == Type::Record || owner == Type::IntRange) return composite(receiver).fields[info.target];
                if (owner.kind == Type::List || owner.kind == Type::FrozenList) return static_cast<std::uint64_t>(composite(receiver).fields.size());
                if (item.name.text == "codePoint") return receiver;
                auto& text = std::get<std::string>(scalarValue(receiver, fn, site));
                if (item.name.text == "byteSize" || fn.expressions[item.receiver].type == Type::Bytes) return static_cast<std::uint64_t>(text.size());
                std::uint64_t count = 0;
                for (std::size_t offset = 0; offset < text.size(); ++count) offset += utf8::sequenceWidth(static_cast<unsigned char>(text[offset]));
                return count;
            } else if constexpr (std::is_same_v<Node, QualifiedMember>) {
                if (info.callee == Callee::Variant)
                    return makeComposite(fn, site, rawType(fn, id), VariantTag{rawType(fn, id).id, static_cast<unsigned>(info.target)});
                std::abort();
            } else if constexpr (std::is_same_v<Node, EmptyOption>) {
                return makeComposite(fn, site, rawType(fn, id), SumTag::None);
            } else if constexpr (std::is_same_v<Node, RangeValue>) {
                return makeComposite(fn, site, Type::IntRange, std::monostate{}, {sub(item.start), sub(item.end), EvalValue{ConstScalar{item.inclusive}}});
            } else if constexpr (std::is_same_v<Node, ListLiteral>) {
                std::vector<EvalValue> values;
                values.reserve(item.elements.size());
                for (auto element : item.elements) values.push_back(sub(element));
                return makeComposite(fn, site, rawType(fn, id), std::monostate{}, std::move(values));
            } else if constexpr (std::is_same_v<Node, Index>) {
                auto receiver = sub(item.receiver);
                auto indexValue = sub(item.offset);
                auto owner = fn.expressions[item.receiver].type;
                if (owner.kind == Type::List || owner.kind == Type::FrozenList) return composite(receiver).fields[listIndex(receiver, indexValue, fn, item.site)];
                auto offset = integer(Type::Int, scalarValue(indexValue, fn, site)).getSExtValue();
                const auto& text = std::get<std::string>(scalarValue(receiver, fn, site));
                if (offset < 0) error(sources, fn, site, "LYCT0003", "编译期索引越界");
                if (owner == Type::Bytes) {
                    if (static_cast<std::uint64_t>(offset) >= text.size()) error(sources, fn, site, "LYCT0003", "编译期索引越界");
                    return static_cast<std::uint64_t>(static_cast<unsigned char>(text[static_cast<std::size_t>(offset)]));
                }
                std::size_t byte = 0;
                for (std::int64_t index = 0; index < offset; ++index) {
                    if (byte >= text.size()) error(sources, fn, site, "LYCT0003", "编译期索引越界");
                    byte += utf8::sequenceWidth(static_cast<unsigned char>(text[byte]));
                }
                if (byte >= text.size()) error(sources, fn, site, "LYCT0003", "编译期索引越界");
                auto width = utf8::sequenceWidth(static_cast<unsigned char>(text[byte]));
                return static_cast<std::uint64_t>(utf8::decode(text.data() + byte, width));
            } else if constexpr (std::is_same_v<Node, Propagate>) {
                auto value = sub(item.operand);
                auto type = fn.expressions[item.operand].type;
                const auto& operand = composite(value);
                auto tag = std::get<SumTag>(operand.tag);
                if (successTag(tag)) return operand.fields.front();
                auto target = frame.result;
                if (type.kind == Type::Result) {
                    auto sourceFailure = module.sumTypes[type.id].failure;
                    auto targetFailure = module.sumTypes[target.id].failure;
                    if (sourceFailure != targetFailure || info.errorConversion || info.errorDisplay.method)
                        error(sources, fn, site, "LYCT0001", "编译期 ? 暂不支持错误类型转换");
                }
                auto failureTag = target.kind == Type::Option ? SumTag::None : SumTag::Err;
                throw Returned{makeComposite(fn, site, target, failureTag, operand.fields)};
            } else if constexpr (std::is_same_v<Node, Block>) {
                EvalValue tail;
                for (const auto& statement : item.statements) tail = sub(statement.expression);
                return rawType(fn, id) == Type::Unit ? EvalValue{ConstScalar{}} : std::move(tail);
            } else if constexpr (std::is_same_v<Node, Conditional>) {
                EvalValue branch;
                if (std::get<bool>(scalarValue(sub(item.condition), fn, site))) branch = sub(item.yes);
                else if (item.no) branch = sub(*item.no);
                return rawType(fn, id) == Type::Unit ? EvalValue{ConstScalar{}} : std::move(branch);
            } else if constexpr (std::is_same_v<Node, Match>) {
                auto subject = sub(item.subject);
                for (std::size_t index = 0; index < item.arms.size(); ++index) {
                    const auto& arm = item.arms[index];
                    PatternBindings bindings;
                    if (!matches(fn, site, info.patterns[index], subject, fn.expressions[item.subject].type, bindings)) continue;
                    for (auto& [slot, value] : bindings) initialize(fn, site, frame, slot, std::move(value));
                    if (arm.guard && !std::get<bool>(scalarValue(sub(*arm.guard), fn, site))) continue;
                    auto value = sub(arm.value);
                    return rawType(fn, id) == Type::Unit ? EvalValue{ConstScalar{}} : std::move(value);
                }
                error(sources, fn, site, "LYT0012", "match 未穷尽，编译期值未命中任何分支");
            } else if constexpr (std::is_same_v<Node, While>) {
                for (;;) {
                    auto condition = sub(item.condition);
                    if (item.pattern) {
                        PatternBindings bindings;
                        if (!matches(fn, item.pattern->span, info.patterns.front(), condition, fn.expressions[item.condition].type, bindings)) break;
                        for (auto& [slot, bound] : bindings) initialize(fn, site, frame, slot, std::move(bound));
                    } else if (!std::get<bool>(scalarValue(condition, fn, site))) break;
                    try { sub(item.body); } catch (Jump jump) { if (jump == Jump::Break) break; }
                }
            } else if constexpr (std::is_same_v<Node, For>) {
                auto input = sub(item.input);
                auto iterate = [&](EvalValue element) {
                    PatternBindings bindings;
                    if (!matches(fn, item.item.span, info.patterns.front(), element, info.iteration->element, bindings))
                        error(sources, fn, item.item.span, "LYT0012", "for 解构必须匹配所有可能的元素");
                    for (auto& [slot, bound] : bindings) initialize(fn, site, frame, slot, std::move(bound));
                    try { sub(item.body); } catch (Jump jump) { return jump != Jump::Break; }
                    return true;
                };
                if (fn.expressions[item.input].type == Type::IntRange) {
                    auto [start, end, inclusive] = rangeParts(input, fn, site);
                    for (auto current = start; current < end || (inclusive && current == end); ++current) {
                        if (!iterate(static_cast<std::uint64_t>(current)) || current == end) break;
                    }
                } else if (const auto& plan = info.iteration->methods) {
                    auto cursor = invoke(module.functions[plan->open], {input}, fn, site);
                    for (;;) {
                        auto next = invoke(module.functions[plan->next], {cursor}, fn, site);
                        auto& option = composite(next);
                        if (std::get<SumTag>(option.tag) == SumTag::None || !iterate(option.fields.front())) break;
                    }
                } else if (fn.expressions[item.input].type.kind == Type::List || fn.expressions[item.input].type.kind == Type::FrozenList) {
                    auto count = composite(input).fields.size();
                    for (std::size_t index = 0; index < count; ++index) {
                        auto current = listIndex(input, static_cast<std::uint64_t>(index), fn, site);
                        if (!iterate(composite(input).fields[current])) break;
                    }
                } else {
                    const auto& text = std::get<std::string>(scalarValue(input, fn, site));
                    auto decode = fn.expressions[item.input].type == Type::String;
                    for (std::size_t offset = 0; offset < text.size();) {
                        auto first = static_cast<unsigned char>(text[offset]);
                        auto width = decode ? utf8::sequenceWidth(first) : 1;
                        auto element = decode ? utf8::decode(text.data() + offset, width) : first;
                        if (!iterate(static_cast<std::uint64_t>(element))) break;
                        offset += width;
                    }
                }
            } else if constexpr (std::is_same_v<Node, Jump>) throw item;
            else if constexpr (std::is_same_v<Node, Return>) throw Returned{item.value ? sub(*item.value) : EvalValue{ConstScalar{}}};
            else if constexpr (std::is_same_v<Node, Unary>) {
                auto operand = sub(item.operand);
                if (item.op == Op::Not) return !std::get<bool>(scalarValue(operand, fn, site));
                auto type = fn.expressions[item.operand].type;
                if (item.op == Op::Complement) return (~integer(type, scalarValue(operand, fn, site))).getZExtValue();
                if (type.kind == Type::Real) return -std::get<double>(scalarValue(operand, fn, site));
                return operation(fn, item.site, Op::Sub, type, std::uint64_t{0}, operand);
            } else if constexpr (std::is_same_v<Node, Binary>) {
                auto op = item.op;
                auto type = fn.expressions[item.left].type;
                if (assignment(op)) {
                    if (type == Type::Never) return sub(item.left);
                    auto place = address(fn, item.left, frame);
                    auto before = EvalValue{};
                    if (op != Op::Set) {
                        auto* read = &cell(place, fn, site);
                        for (auto slot : place.path) read = &composite(*read).fields[slot];
                        before = *read;
                    }
                    auto after = sub(item.right);
                    if (op != Op::Set) {
                        after = operation(fn, item.site, compoundOperator(op), type, before, after);
                        if (const auto* scalar = std::get_if<ConstScalar>(&after))
                            if (const auto* text = std::get_if<std::string>(scalar)) chargeText(fn, site, text->size());
                    }
                    auto& destination = cell(place, fn, site);
                    destination = replace(fn, site, destination, place.path, 0, std::move(after));
                } else {
                    auto left = sub(item.left);
                    if (op == Op::And) return std::get<bool>(scalarValue(left, fn, site)) && std::get<bool>(scalarValue(sub(item.right), fn, site));
                    if (op == Op::Or) return std::get<bool>(scalarValue(left, fn, site)) || std::get<bool>(scalarValue(sub(item.right), fn, site));
                    auto right = sub(item.right);
                    return operation(fn, item.site, op, type, left, right);
                }
            } else if constexpr (std::is_same_v<Node, Call>) {
                if (info.callee == Callee::FreezeList) {
                    auto source = sub(item.arguments.front().value);
                    return makeComposite(fn, site, rawType(fn, id), std::monostate{}, composite(source).fields);
                }
                if (item.receiver && info.callee != Callee::Variant && fn.expressions[*item.receiver].type == Type::Never) return sub(*item.receiver);
                if (info.callee == Callee::Indirect) {
                    auto value = item.function ? sub(*item.function) : item.receiver ? composite(sub(*item.receiver)).fields[info.target]
                        : info.constantRef ? this->value(*info.constantRef, fn, site) : frame.cell(info.target);
                    if (info.narrowed) value = composite(value).fields.front();
                    const auto& closure = closures.at(std::get<ConstRef>(value).index);
                    auto skip = static_cast<std::size_t>(closure.receiver.has_value());
                    std::vector<EvalValue> args(item.arguments.size() + skip);
                    if (closure.receiver) args.front() = *closure.receiver;
                    for (std::size_t index = 0; index < item.arguments.size(); ++index)
                        args[info.argumentSlots[index] + skip] = sub(item.arguments[index].value);
                    return invoke(closure.function, std::move(args), fn, item.name.span, closure.lambda ? &closure : nullptr);
                }
                if (item.receiver && info.callee != Callee::Function && info.callee != Callee::Variant) {
                    auto receiver = sub(*item.receiver);
                    auto from = fn.expressions[*item.receiver].type;
                    if (info.callee == Callee::NumericAbs || info.callee == Callee::NumericMin || info.callee == Callee::NumericMax ||
                        info.callee == Callee::NumericClamp || info.callee == Callee::IsFinite || info.callee == Callee::IsNaN || info.callee == Callee::IsInfinite) {
                        auto first = item.arguments.empty() ? EvalValue{} : sub(item.arguments.front().value);
                        auto second = item.arguments.size() < 2 ? EvalValue{} : sub(item.arguments.back().value);
                        bool reversed = item.arguments.size() == 2 && info.argumentSlots.front() == 1;
                        return numericMember(fn, item.name.span, from, info.callee, scalarValue(receiver, fn, site),
                            scalarValue(reversed ? second : first, fn, site), scalarValue(reversed ? first : second, fn, site));
                    }
                    if (info.callee == Callee::SumPresent || info.callee == Callee::SumAbsent || info.callee == Callee::SumUnwrap ||
                        info.callee == Callee::CarryFailure) {
                        const auto& sum = composite(receiver);
                        if (info.callee == Callee::CarryFailure)
                            return makeComposite(fn, site, rawType(fn, id), SumTag::Err, {sum.fields.front()});
                        auto present = successTag(std::get<SumTag>(sum.tag));
                        if (info.callee == Callee::SumPresent) return present;
                        if (info.callee == Callee::SumAbsent) return !present;
                        auto message = item.arguments.empty() ? std::string(from.kind == Type::Option ? "unwrap on None" : "unwrap on Err")
                            : std::get<std::string>(scalarValue(sub(item.arguments.front().value), fn, site));
                        if (!present) error(sources, fn, item.name.span, "LYCT0003", std::move(message));
                        return composite(receiver).fields.front();
                    }
                    if (info.callee == Callee::ValueEqual || info.callee == Callee::ValueHash || info.callee == Callee::Size) {
                        if (info.callee == Callee::ValueHash) return hash(receiver, from, fn, item.name.span);
                        if (info.callee == Callee::ValueEqual) return equal(receiver, sub(item.arguments.front().value), from, fn, item.name.span);
                        if (from.kind == Type::List || from.kind == Type::FrozenList) return static_cast<std::uint64_t>(composite(receiver).fields.size());
                        const auto& text = std::get<std::string>(scalarValue(receiver, fn, site));
                        if (from == Type::Bytes) return static_cast<std::uint64_t>(text.size());
                        std::uint64_t count = 0;
                        for (std::size_t offset = 0; offset < text.size(); ++count) offset += utf8::sequenceWidth(static_cast<unsigned char>(text[offset]));
                        return count;
                    }
                    if (info.callee == Callee::ListAdd) {
                        auto element = sub(item.arguments.front().value);
                        charge(fn, site, sizeof(EvalValue));
                        composite(receiver).fields.push_back(std::move(element));
                        return {};
                    }
                    if (info.callee == Callee::ListRemoveAt) {
                        auto offset = sub(item.arguments.front().value);
                        auto index = listIndex(receiver, offset, fn, item.name.span);
                        auto& fields = composite(receiver).fields;
                        auto removed = std::move(fields[index]);
                        fields.erase(fields.begin() + index);
                        return removed;
                    }
                    if (info.callee == Callee::ListGetOrNone || info.callee == Callee::ListFirstOrNone || info.callee == Callee::ListLastOrNone) {
                        auto index = info.callee == Callee::ListGetOrNone ? std::get<std::uint64_t>(scalarValue(sub(item.arguments.front().value), fn, site)) : 0;
                        const auto& fields = composite(receiver).fields;
                        if (info.callee == Callee::ListLastOrNone) index = static_cast<std::uint64_t>(fields.size()) - 1;
                        auto target = rawType(fn, id);
                        if (index >= fields.size()) return makeComposite(fn, site, target, SumTag::None);
                        return makeComposite(fn, site, target, SumTag::Some, {fields[index]});
                    }
                    if (info.callee == Callee::ListSlice) {
                        auto [start, end, inclusive] = rangeParts(sub(item.arguments.front().value), fn, item.name.span);
                        const auto& fields = composite(receiver).fields;
                        auto bounds = listSliceBounds(fields.size(), start, end, inclusive);
                        if (!bounds) error(sources, fn, item.name.span, "LYCT0003", "编译期列表切片越界");
                        auto first = fields.begin() + bounds->start;
                        return makeComposite(fn, site, from, std::monostate{}, {first, first + bounds->count});
                    }
                    if (info.callee == Callee::BytesSlice) {
                        auto [start, end, inclusive] = rangeParts(sub(item.arguments.front().value), fn, item.name.span);
                        const auto& bytes = std::get<std::string>(scalarValue(receiver, fn, site));
                        auto bounds = listSliceBounds(bytes.size(), start, end, inclusive);
                        if (!bounds) error(sources, fn, item.name.span, "LYCT0003", "编译期字节切片越界");
                        return bytes.substr(bounds->start, bounds->count);
                    }
                    if (info.callee == Callee::ToString) return display(scalarValue(receiver, fn, site), from);
                    if (info.callee == Callee::ToBytes) return receiver;
                    auto target = rawType(fn, id);
                    if (info.callee == Callee::ToIntegerChecked) {
                        auto converted = convertInteger(from, target, scalarValue(receiver, fn, site));
                        if (!converted) return makeComposite(fn, site, target, SumTag::None);
                        return makeComposite(fn, site, target, SumTag::Some, {EvalValue{ConstScalar{*converted}}});
                    }
                    if (from.kind == Type::Real) return asDouble(real(target, std::get<double>(scalarValue(receiver, fn, site))), target);
                    llvm::APFloat result(numberWidth(target) == 32 ? llvm::APFloat::IEEEsingle() : llvm::APFloat::IEEEdouble());
                    result.convertFromAPInt(integer(from, scalarValue(receiver, fn, site)), from.kind == Type::Int, llvm::APFloat::rmNearestTiesToEven);
                    return asDouble(result, target);
                }
                if (info.callee == Callee::Variant) {
                    auto type = rawType(fn, id);
                    const auto& fields = module.enums[type.id].payloads[info.target];
                    std::vector<EvalValue> values(fields.size());
                    for (std::size_t index = 0; index < item.arguments.size(); ++index)
                        values[info.argumentSlots[index]] = sub(item.arguments[index].value);
                    return makeComposite(fn, site, type, VariantTag{type.id, static_cast<unsigned>(info.target)}, std::move(values));
                }
                if (info.callee == Callee::Sum) {
                    auto type = rawType(fn, id);
                    auto tag = static_cast<SumTag>(info.target);
                    std::vector<EvalValue> values;
                    if (tag != SumTag::None) values.push_back(sub(item.arguments.front().value));
                    return makeComposite(fn, site, type, tag, std::move(values));
                }
                if (info.callee == Callee::Constructor) {
                    auto type = rawType(fn, id);
                    const auto& record = module.aggregates[info.target];
                    std::vector<EvalValue> values(record.fields.size());
                    auto skip = static_cast<std::size_t>(item.receiver.has_value());
                    if (item.receiver) values[0] = sub(*item.receiver);
                    for (std::size_t index = 0; index < item.arguments.size(); ++index)
                        values[info.argumentSlots[index] + skip] = sub(item.arguments[index].value);
                    for (auto slot : info.omittedDefaults) {
                        const auto& field = record.syntax.fields[slot];
                        if (field.defaultValue && field.defaultValue->emptyOption) {
                            values[slot] = makeComposite(fn, item.name.span, record.fields[slot], SumTag::None);
                        } else {
                            const auto& helper = module.functions[*record.defaultFunctions[slot]];
                            values[slot] = invoke(helper, {values.begin(), values.begin() + slot}, fn, item.name.span);
                        }
                    }
                    return makeComposite(fn, site, type, std::monostate{}, std::move(values));
                }
                const auto& target = module.functions[info.target];
                std::vector<EvalValue> args(target.syntax.parameters.size());
                auto skip = static_cast<std::size_t>(item.receiver.has_value());
                if (item.receiver) args[0] = sub(*item.receiver);
                for (std::size_t index = 0; index < item.arguments.size(); ++index) {
                    auto evaluated = sub(item.arguments[index].value);
                    args[info.argumentSlots[index] + skip] = std::move(evaluated);
                }
                for (auto slot : info.omittedDefaults) {
                    const auto& helper = module.functions[*target.defaultFunctions[slot]];
                    args[slot] = invoke(helper, {args.begin(), args.begin() + slot}, fn, item.name.span);
                }
                return invoke(target, std::move(args), fn, item.name.span);
            } else std::abort();
            return {};
        }, node);
        if (const auto* scalar = std::get_if<ConstScalar>(&result))
            if (const auto* text = std::get_if<std::string>(scalar)) chargeText(fn, site, text->size());
        if (info.sourceType) {
            auto from = *info.sourceType;
            auto to = info.type;
            if (to.kind == Type::Option && module.sumTypes[to.id].success == from)
                return makeComposite(fn, site, to, SumTag::Some, {std::move(result)});
            if (integerType(from) && integerType(to)) {
                auto bits = integer(from, scalarValue(result, fn, site));
                auto converted = from.kind == Type::Int ? bits.sextOrTrunc(numberWidth(to)) : bits.zextOrTrunc(numberWidth(to));
                return converted.getZExtValue();
            }
            if (from.kind == Type::Real && to.kind == Type::Real)
                return asDouble(real(to, std::get<double>(scalarValue(result, fn, site))), to);
        }
        return result;
    }
};

}

void evaluateConstants(CheckedModule& module, const std::vector<Source>& sources, const std::function<void(std::size_t)>& requireBody) {
    Validation state{module, sources, ValueTypes{module, {}}, std::vector<bool>(module.functions.size()), {}};
    if (requireBody) state.requireBody = &requireBody;
    for (std::size_t index = 0; index < module.functions.size(); ++index) {
        const auto& fn = module.functions[index];
        if (fn.syntax.constant != ConstKind::None) Validator{state, fn}.validateFunction(index, fn.syntax.name.span);
    }
    for (const auto& block : module.blockConstants) {
        const auto& fn = module.functions[block.function];
        if (!state.types.supports(block.type, false))
            error(sources, fn, block.name.span, "LYCT0001", "块内常量必须保存不可变值；List、Class 和函数值只能暂存");
        Validator{state, fn}.visit(block.initializer);
    }
    for (std::size_t cursor = 0; cursor < state.pending.size(); ++cursor) {
        const auto index = state.pending[cursor];
        Validator{state, module.functions[index]}.run();
    }
    module.constants.resize(module.functions.size());
    std::vector<bool> active(module.functions.size() + module.blockConstants.size());
    for (std::size_t index = 0; index < module.functions.size(); ++index) {
        const auto& fn = module.functions[index];
        if (fn.syntax.constant == ConstKind::Value) Evaluation{module, sources, active}.value({ConstantUse::Global, index}, fn, fn.syntax.name.span);
    }
    for (std::size_t index = 0; index < module.blockConstants.size(); ++index) {
        const auto& block = module.blockConstants[index];
        Evaluation{module, sources, active}.value({ConstantUse::Block, index}, module.functions[block.function], block.name.span);
    }
}

void evaluateMacro(CheckedModule& module, const std::vector<Source>& sources, MacroExecution& macro, std::size_t entry,
    const std::function<void(std::size_t)>& checkBody) {
    const auto& fn = module.functions[entry];
    auto syntax = fn.result;
    if (syntax.kind != Type::Record || module.aggregates[syntax.id].syntax.file != macro.prelude ||
        !module.aggregates[syntax.id].syntax.name.text.ends_with("::Syntax") || fn.locals.size() != 2 ||
        fn.locals[0].type != Type::String || fn.locals[1].type != Type::String)
        error(sources, fn, fn.syntax.name.span, "LYM0002", "宏入口需要签名 (target: String, argument: String): Syntax");
    Validation state{module, sources, ValueTypes{module, {}}, {}, {}, &macro};
    Validator{state, fn}.validateFunction(entry, fn.syntax.name.span);
    for (std::size_t cursor = 0; cursor < state.pending.size(); ++cursor) {
        auto index = state.pending[cursor];
        checkBody(index);
        Validator{state, module.functions[index]}.run();
    }
    module.constants.resize(module.functions.size());
    std::vector<bool> active(module.functions.size() + module.blockConstants.size());
    Evaluation evaluation{module, sources, active, 50000};
    evaluation.macro = &macro;
    evaluation.syntaxType = syntax;
    for (auto index : state.pending) {
        const auto& item = module.functions[index];
        if (item.syntax.constant == ConstKind::Value) evaluation.value({ConstantUse::Global, index}, item, item.syntax.name.span);
    }
    for (auto index : state.blocks) {
        const auto& block = module.blockConstants[index];
        evaluation.value({ConstantUse::Block, index}, module.functions[block.function], block.name.span);
    }
    auto result = evaluation.invoke(fn, {ConstScalar{macro.target}, ConstScalar{macro.argument}}, fn, fn.syntax.name.span);
    macro.output.index = std::get<std::uint64_t>(std::get<ConstScalar>(evaluation.composite(result).fields.front()));
}

}
