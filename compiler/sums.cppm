module;
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

export module lyra.sums;
import lyra.source;

export namespace lyra {

enum class SumTag { None, Some, Ok, Err };
struct PatternTag { SumTag tag; Span span; };
struct PatternBinding { std::string name; Span span; };
struct SumPattern { Span span; std::vector<PatternTag> tags; std::optional<PatternBinding> binding; };

std::optional<SumTag> sumTag(std::string_view text) {
    if (text == "None") return SumTag::None;
    if (text == "Some") return SumTag::Some;
    if (text == "Ok") return SumTag::Ok;
    if (text == "Err") return SumTag::Err;
    return std::nullopt;
}

std::string_view tagName(SumTag tag) {
    switch (tag) {
    case SumTag::None: return "None";
    case SumTag::Some: return "Some";
    case SumTag::Ok: return "Ok";
    case SumTag::Err: return "Err";
    }
    throw std::logic_error("invalid sum tag");
}

bool optionTag(SumTag tag) { return tag == SumTag::None || tag == SumTag::Some; }
bool successTag(SumTag tag) { return tag == SumTag::Some || tag == SumTag::Ok; }
unsigned payloadSlot(SumTag tag) { return tag == SumTag::Err ? 2 : 1; }

template<class Type> struct SumCase { SumTag tag; Type payload; };

template<class Type, class Cases> std::optional<std::string> missingPattern(
        Type type, const std::vector<const SumPattern*>& patterns, const Cases& cases, std::size_t depth = 0) {
    for (auto* pattern : patterns) if (pattern->tags.size() == depth) return std::nullopt;
    auto alternatives = cases(type);
    if (!alternatives) return "_";
    for (const auto& branch : *alternatives) {
        std::vector<const SumPattern*> selected;
        for (auto* pattern : patterns) if (pattern->tags[depth].tag == branch.tag) selected.push_back(pattern);
        std::string name(tagName(branch.tag));
        if (selected.empty()) return branch.tag == SumTag::None ? name : name + "(_)";
        if (branch.tag != SumTag::None) {
            if (auto missing = missingPattern(branch.payload, selected, cases, depth + 1)) return name + "(" + *missing + ")";
        }
    }
    return std::nullopt;
}

}
