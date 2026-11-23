module;
#include <optional>
#include <stdexcept>
#include <string_view>

export module lyra.sums;

export namespace lyra {

enum class SumTag { None, Some, Ok, Err };

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

}
