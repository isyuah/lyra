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
struct Constructor { PatternConstructor key; std::span<const Type> fields; };

std::vector<Constructor> constructors(const CheckedModule& module, Type type) {
    switch (type.kind) {
        case Type::Bool: return {{true, {}}, {false, {}}};
        case Type::Option: return {{SumTag::None, {}}, {SumTag::Some, {&module.sumTypes[type.id].success, 1}}};
        case Type::Result: return {{SumTag::Ok, {&module.sumTypes[type.id].success, 1}}, {SumTag::Err, {&module.sumTypes[type.id].failure, 1}}};
        case Type::Enum: {
            std::vector<Constructor> result;
            const auto& payloads = module.enums[type.id].payloads;
            for (unsigned index = 0; index < payloads.size(); ++index) result.push_back({VariantTag{type.id, index}, payloads[index]});
            return result;
        }
        case Type::Record:
            if (!module.aggregates[type.id].syntax.reference) return {{PatternProduct{}, module.aggregates[type.id].fields}};
            break;
        default: break;
    }
    return {};
}

bool wildcard(const CheckedPattern* pattern) {
    return !pattern || std::holds_alternative<std::monostate>(pattern->node) || std::holds_alternative<PatternSlot>(pattern->node);
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

export std::optional<std::string> missingPattern(const CheckedModule& module, Type subject, std::span<const CheckedPattern* const> patterns) {
    std::vector<PatternRow> rows;
    for (const auto* pattern : patterns) rows.push_back({pattern});
    if (auto missing = uncovered(module, {&subject, 1}, rows)) return std::move(missing->front());
    return std::nullopt;
}

}
