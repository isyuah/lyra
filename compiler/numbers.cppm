module;
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

export module lyra.numbers;
import lyra.check;

export namespace lyra {

constexpr Type number(Type::Kind kind, unsigned width) { return {kind, width == 64 ? 0 : width}; }
constexpr unsigned numberWidth(Type type) { return type.id ? static_cast<unsigned>(type.id) : 64; }
constexpr bool integerType(Type type) { return type.kind == Type::Int || type.kind == Type::UInt; }
constexpr bool numericType(Type type) { return integerType(type) || type.kind == Type::Real; }

std::optional<Type> numberType(std::string_view name) {
    if (name == "Float" || name == "Float64") return Type::Real;
    if (name == "Float32") return number(Type::Real, 32);
    auto kind = name.starts_with('U') ? Type::UInt : Type::Int;
    auto prefix = kind == Type::UInt ? "UInt" : "Int";
    if (!name.starts_with(prefix)) return std::nullopt;
    auto width = name.substr(std::string_view(prefix).size());
    if (width.empty() || width == "64") return kind;
    if (width == "32") return number(kind, 32);
    if (width == "16") return number(kind, 16);
    if (width == "8") return number(kind, 8);
    return std::nullopt;
}

std::string numberName(Type type) {
    if (type.kind == Type::Real) return "Float" + std::to_string(numberWidth(type));
    auto prefix = type.kind == Type::UInt ? "UInt" : "Int";
    return type.id ? prefix + std::to_string(numberWidth(type)) : std::string(prefix);
}

constexpr bool losslessNumber(Type from, Type to) {
    if (!numericType(from) || !numericType(to)) return false;
    if (from.kind == to.kind) return numberWidth(from) <= numberWidth(to);
    return from.kind == Type::UInt && to.kind == Type::Int && numberWidth(from) < numberWidth(to);
}

std::optional<Type> commonInteger(Type left, Type right) {
    if (!integerType(left) || !integerType(right)) return std::nullopt;
    auto kind = left.kind == Type::Int || right.kind == Type::Int ? Type::Int : Type::UInt;
    for (auto width : {8U, 16U, 32U, 64U}) {
        auto candidate = number(kind, width);
        if (losslessNumber(left, candidate) && losslessNumber(right, candidate)) return candidate;
    }
    return std::nullopt;
}

constexpr bool represents(Type type, IntegerLiteral literal) {
    auto width = numberWidth(type);
    if (type.kind == Type::UInt) return !literal.negative && (width == 64 || literal.magnitude < (std::uint64_t{1} << width));
    auto limit = std::uint64_t{1} << (width - 1);
    return literal.magnitude < limit || (literal.negative && literal.magnitude == limit);
}

constexpr std::uint64_t integerBits(IntegerLiteral literal) {
    return literal.negative ? std::uint64_t{0} - literal.magnitude : literal.magnitude;
}

}
