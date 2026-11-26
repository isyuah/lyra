#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <set>
#include <string_view>

#include <lyra/utf8.hpp>

namespace lyra::httpRoute {

inline constexpr std::size_t maxTarget = 8192;

inline int hex(char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

inline std::optional<std::string_view> decode(std::string_view input, bool query, std::array<char, maxTarget>& bytes) {
    std::size_t size = 0;
    for (std::size_t index = 0; index < input.size(); ++index) {
        auto byte = input[index];
        if (byte == '%') {
            if (input.size() - index < 3) return {};
            auto high = hex(input[index + 1]), low = hex(input[index + 2]);
            if (high < 0 || low < 0) return {};
            byte = static_cast<char>(high * 16 + low);
            index += 2;
        } else if (query && byte == '+') byte = ' ';
        bytes[size++] = byte;
    }
    if (!utf8::valid(bytes.data(), size)) return {};
    return std::string_view(bytes.data(), size);
}

template<class Visit>
bool split(std::string_view input, char separator, Visit visit) {
    for (;;) {
        auto end = input.find(separator);
        if (!visit(input.substr(0, end))) return false;
        if (end == input.npos) return true;
        input.remove_prefix(end + 1);
    }
}

inline bool identifier(std::string_view input) {
    auto letter = [](char byte) { return byte == '_' || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z'); };
    return !input.empty() && letter(input.front()) && std::ranges::all_of(input.substr(1), [&](char byte) {
        return letter(byte) || (byte >= '0' && byte <= '9');
    });
}

template<class Visit>
bool parse(std::string_view pattern, Visit visit) {
    if (pattern.size() > maxTarget || !pattern.starts_with('/') || pattern.find_first_of("?#") != pattern.npos) return false;
    std::set<std::string_view> parameters;
    std::array<char, maxTarget> bytes;
    return split(pattern.substr(1), '/', [&](std::string_view part) {
        if (part.find_first_of("{}") != part.npos) {
            if (!part.starts_with('{') || !part.ends_with('}')) return false;
            auto name = part.substr(1, part.size() - 2);
            if (!identifier(name) || !parameters.insert(name).second) return false;
            visit(true, name);
        } else {
            auto value = decode(part, false, bytes);
            if (!value) return false;
            visit(false, *value);
        }
        return true;
    });
}

}
