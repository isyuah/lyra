module;
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

export module lyra.patterns;
import lyra.check;

namespace lyra {
namespace {

using PatternRow = std::vector<const CheckedPattern*>;
using Witness = std::vector<std::string>;
struct Constructor { PatternConstructor key; std::vector<Type> fields; };

std::vector<Constructor> constructors(const CheckedModule& module, Type type) {
    switch (type.kind) {
        case Type::Bool: return {{true, {}}, {false, {}}};
        case Type::Option: return {{SumTag::None, {}}, {SumTag::Some, {module.sumTypes[type.id].success}}};
        case Type::Result: return {{SumTag::Ok, {module.sumTypes[type.id].success}}, {SumTag::Err, {module.sumTypes[type.id].failure}}};
        case Type::Enum: {
            std::vector<Constructor> result;
            const auto& payloads = module.enums[type.id].payloads;
            for (unsigned index = 0; index < payloads.size(); ++index) result.push_back({VariantTag{type.id, index}, payloads[index]});
            return result;
        }
        case Type::Record:
            if (!module.aggregates[type.id].syntax.reference) return {{PatternProduct{}, module.aggregates[type.id].fields}};
            break;
        case Type::List: return {{ListTag{false, module.listElements[type.id], type}, {}}, {ListTag{true, module.listElements[type.id], type}, {module.listElements[type.id], type}}};
        default: break;
    }
    return {};
}

bool wildcard(const CheckedPattern* pattern) {
    return !pattern || std::holds_alternative<std::monostate>(pattern->node) || std::holds_alternative<PatternSlot>(pattern->node) ||
        std::holds_alternative<PatternListRest>(pattern->node);
}

bool plainDestructure(const CheckedPattern& pattern) {
    if (wildcard(&pattern)) return true;
    auto* constructor = std::get_if<PatternConstructor>(&pattern.node);
    return constructor && (std::holds_alternative<PatternProduct>(*constructor) || std::holds_alternative<PatternClass>(*constructor)) &&
        std::ranges::all_of(pattern.parts, plainDestructure);
}

std::optional<CheckedPattern> constantPattern(const CheckedModule& module, const ConstValue& value, Type type) {
    if (module.valueOverrides.contains({type, ValueOperation::Equal})) return {};
    if (const auto* scalar = std::get_if<ConstScalar>(&value)) {
        if (const auto* flag = std::get_if<bool>(scalar)) return CheckedPattern{PatternConstructor(*flag)};
        if (const auto* integer = std::get_if<std::uint64_t>(scalar)) return CheckedPattern{*integer};
        if (const auto* text = std::get_if<std::string>(scalar)) return CheckedPattern{StringLiteral{*text}};
        return {};
    }
    const auto& composite = module.constantComposites[std::get<ConstRef>(value).index];
    if (type.kind == Type::Record) {
        if (module.aggregates[type.id].syntax.reference || !std::holds_alternative<std::monostate>(composite.tag)) return {};
        CheckedPattern result{PatternConstructor(PatternProduct{}), {}};
        const auto& fields = module.aggregates[type.id].fields;
        for (std::size_t slot = 0; slot < fields.size(); ++slot) {
            auto part = constantPattern(module, composite.fields[slot], fields[slot]);
            if (!part) return {};
            result.parts.push_back(std::move(*part));
        }
        return result;
    }
    if (type.kind == Type::Enum) {
        const auto* tag = std::get_if<VariantTag>(&composite.tag);
        if (!tag) return {};
        CheckedPattern result{PatternConstructor(*tag), {}};
        const auto& fields = module.enums[type.id].payloads[tag->index];
        for (std::size_t slot = 0; slot < fields.size(); ++slot) {
            auto part = constantPattern(module, composite.fields[slot], fields[slot]);
            if (!part) return {};
            result.parts.push_back(std::move(*part));
        }
        return result;
    }
    if (type.kind == Type::Option || type.kind == Type::Result) {
        const auto* tag = std::get_if<SumTag>(&composite.tag);
        if (!tag) return {};
        CheckedPattern result{PatternConstructor(*tag), {}};
        if (*tag != SumTag::None) {
            const auto& shape = module.sumTypes[type.id];
            auto field = successTag(*tag) ? shape.success : shape.failure;
            auto part = constantPattern(module, composite.fields.front(), field);
            if (!part) return {};
            result.parts.push_back(std::move(*part));
        }
        return result;
    }
    return {};
}

CheckedPattern normalize(const CheckedModule& module, const CheckedPattern& pattern, Type type, PatternUse use) {
    if (use == PatternUse::Binding && plainDestructure(pattern)) return {};
    if (const auto* constant = std::get_if<PatternConstant>(&pattern.node))
        if (module.constants[constant->target])
            if (auto resolved = constantPattern(module, *module.constants[constant->target], constant->type)) return std::move(*resolved);
    auto result = pattern;
    if (std::holds_alternative<CheckedAlternatives>(pattern.node)) {
        for (auto& part : result.parts) part = normalize(module, part, type, use);
    } else if (const auto* constructor = std::get_if<PatternConstructor>(&pattern.node)) {
        if (const auto* list = std::get_if<ListTag>(constructor); list && list->cons) {
            result.parts[0] = normalize(module, result.parts[0], list->element, use);
            result.parts[1] = normalize(module, result.parts[1], list->list, use);
        } else if (const auto* variant = std::get_if<VariantTag>(constructor)) {
            const auto& fields = module.enums[type.id].payloads[variant->index];
            for (std::size_t slot = 0; slot < result.parts.size(); ++slot) result.parts[slot] = normalize(module, result.parts[slot], fields[slot], use);
        } else if (const auto* tag = std::get_if<SumTag>(constructor); tag && *tag != SumTag::None) {
            const auto& shape = module.sumTypes[type.id];
            auto field = successTag(*tag) ? shape.success : shape.failure;
            result.parts.front() = normalize(module, result.parts.front(), field, use);
        } else if (std::holds_alternative<PatternProduct>(*constructor) || std::holds_alternative<PatternClass>(*constructor)) {
            const auto& fields = module.aggregates[type.id].fields;
            for (std::size_t slot = 0; slot < result.parts.size(); ++slot) result.parts[slot] = normalize(module, result.parts[slot], fields[slot], use);
        }
    }
    return result;
}

struct IntegerInterval { std::uint64_t first, last; bool empty; };

unsigned integerWidth(Type type) { return type.id ? static_cast<unsigned>(type.id) : 64; }

std::uint64_t integerMask(Type type) {
    auto width = integerWidth(type);
    return width == 64 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t{1} << width) - 1;
}

std::uint64_t orderedInteger(std::uint64_t bits, Type type) {
    bits &= integerMask(type);
    if (type.kind == Type::UInt) return bits;
    return bits ^ (std::uint64_t{1} << (integerWidth(type) - 1));
}

IntegerInterval integerInterval(const CheckedPattern& pattern, Type type) {
    if (const auto* value = std::get_if<std::uint64_t>(&pattern.node)) {
        auto point = orderedInteger(*value, type);
        return {point, point, false};
    }
    const auto* range = std::get_if<CheckedIntegerRange>(&pattern.node);
    if (!range) return {0, 0, true};
    auto first = orderedInteger(range->lower, type);
    auto last = orderedInteger(range->upper, type);
    if (range->inclusive) return {first, last, first > last};
    return {first, last ? last - 1 : 0, first >= last};
}

bool covers(const CheckedPattern& pattern, Type type, std::uint64_t point) {
    auto interval = integerInterval(pattern, type);
    return !interval.empty && interval.first <= point && point <= interval.last;
}

std::uint64_t integerMaximum(Type type) { return integerMask(type); }

std::string integerWitness(Type type, std::uint64_t ordered) {
    if (type.kind == Type::UInt) return std::to_string(ordered);
    auto sign = std::uint64_t{1} << (integerWidth(type) - 1);
    if (ordered >= sign) return std::to_string(ordered - sign);
    return "-" + std::to_string(sign - ordered);
}

std::string display(const CheckedModule& module, Type type, const Constructor& constructor, std::span<const std::string> fields) {
    if (const auto* list = std::get_if<ListTag>(&constructor.key)) {
        if (!list->cons) return "[]";
        return "[" + fields[0] + ", .." + fields[1] + "]";
    }
    if (const auto* tag = std::get_if<SumTag>(&constructor.key)) {
        std::string name(tagName(*tag));
        return fields.empty() ? name : name + "(" + fields.front() + ")";
    }
    if (const auto* flag = std::get_if<bool>(&constructor.key)) return *flag ? "true" : "false";
    if (const auto* variant = std::get_if<VariantTag>(&constructor.key)) {
        auto text = module.enums[type.id].syntax.cases[variant->index].name.text;
        if (fields.empty()) return text;
        text += '(';
        for (std::size_t slot = 0; slot < fields.size(); ++slot) {
            if (slot) text += ", ";
            text += fields[slot];
        }
        return text + ')';
    }
    const auto& record = module.aggregates[type.id].syntax;
    const auto separator = record.name.text.rfind("::");
    std::string text = record.name.text.substr(separator == std::string::npos ? 0 : separator + 2) + "(";
    bool first = true;
    for (std::size_t slot = 0; slot < fields.size(); ++slot) {
        if (fields[slot] == "_") continue;
        if (!first) text += ", ";
        first = false;
        text += record.fields[slot].name.text + ": " + fields[slot];
    }
    return text + ")";
}

std::optional<Witness> uncovered(const CheckedModule& module, std::span<const Type> types, const std::vector<PatternRow>& rows);

std::optional<Witness> uncoveredIntegers(const CheckedModule& module, std::span<const Type> types, const std::vector<PatternRow>& rows) {
    auto type = types.front();
    auto maximum = integerMaximum(type);
    std::vector<std::uint64_t> starts{0};
    for (const auto& row : rows) {
        const auto* first = row.front();
        if (wildcard(first)) continue;
        auto interval = integerInterval(*first, type);
        if (interval.empty) continue;
        starts.push_back(interval.first);
        if (interval.last < maximum) starts.push_back(interval.last + 1);
    }
    std::ranges::sort(starts);
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    auto tail = types.subspan(1);
    for (auto point : starts) {
        std::vector<PatternRow> selected;
        for (const auto& row : rows) {
            const auto* first = row.front();
            if (!wildcard(first) && !covers(*first, type, point)) continue;
            selected.emplace_back(row.begin() + 1, row.end());
        }
        if (auto missing = uncovered(module, tail, selected)) {
            missing->insert(missing->begin(), integerWitness(type, point));
            return missing;
        }
    }
    return std::nullopt;
}

std::optional<Witness> uncovered(const CheckedModule& module, std::span<const Type> types, const std::vector<PatternRow>& rows) {
    if (rows.empty()) return Witness(types.size(), "_");
    if (std::ranges::any_of(rows, [](const PatternRow& row) { return std::ranges::all_of(row, wildcard); })) return std::nullopt;

    auto alternativeRow = std::ranges::find_if(rows, [](const PatternRow& row) {
        return !row.empty() && std::holds_alternative<CheckedAlternatives>(row.front()->node);
    });
    if (alternativeRow != rows.end()) {
        std::vector<PatternRow> expanded;
        for (const auto& row : rows) {
            if (row.empty() || !std::holds_alternative<CheckedAlternatives>(row.front()->node)) {
                expanded.push_back(row);
                continue;
            }
            for (const auto& part : row.front()->parts) {
                auto alternative = row;
                alternative.front() = &part;
                expanded.push_back(std::move(alternative));
            }
        }
        return uncovered(module, types, expanded);
    }

    auto type = types.front();
    if (type.kind == Type::Int || type.kind == Type::UInt) return uncoveredIntegers(module, types, rows);

    auto alternatives = constructors(module, type);
    auto tail = types.subspan(1);
    if (alternatives.empty() || std::ranges::all_of(rows, [](const PatternRow& row) { return wildcard(row.front()); })) {
        std::vector<PatternRow> defaults;
        for (const auto& row : rows) if (wildcard(row.front())) defaults.emplace_back(row.begin() + 1, row.end());
        if (auto missing = uncovered(module, tail, defaults)) {
            missing->insert(missing->begin(), "_");
            return missing;
        }
        return std::nullopt;
    }

    for (const auto& alternative : alternatives) {
        std::vector<Type> typesInCase(alternative.fields.begin(), alternative.fields.end());
        typesInCase.insert(typesInCase.end(), tail.begin(), tail.end());
        std::vector<PatternRow> selected;
        for (const auto& row : rows) {
            const auto* first = row.front();
            PatternRow head;
            if (wildcard(first)) head.resize(alternative.fields.size());
            else {
                const auto* key = std::get_if<PatternConstructor>(&first->node);
                if (!key || *key != alternative.key) continue;
                for (const auto& field : first->parts) head.push_back(&field);
            }
            head.insert(head.end(), row.begin() + 1, row.end());
            selected.push_back(std::move(head));
        }
        if (auto missing = uncovered(module, typesInCase, selected)) {
            auto fields = std::span(*missing).first(alternative.fields.size());
            auto spelling = display(module, type, alternative, fields);
            missing->erase(missing->begin(), missing->begin() + fields.size());
            missing->insert(missing->begin(), std::move(spelling));
            return missing;
        }
    }
    return std::nullopt;
}

}

export std::optional<std::string> missingPattern(const CheckedModule& module, Type subject, std::span<const CheckedPattern* const> patterns, PatternUse use) {
    std::vector<CheckedPattern> normalized;
    normalized.reserve(patterns.size());
    for (const auto* pattern : patterns) normalized.push_back(normalize(module, *pattern, subject, use));
    std::vector<PatternRow> rows;
    for (const auto& pattern : normalized) rows.push_back({&pattern});
    if (auto missing = uncovered(module, {&subject, 1}, rows)) return std::move(missing->front());
    return std::nullopt;
}

}
