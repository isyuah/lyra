#pragma once

#include <cstddef>
#include <cstdint>

namespace lyra::utf8 {

constexpr bool scalar(std::uint32_t point) { return point <= 0x10ffff && (point < 0xd800 || point > 0xdfff); }
constexpr unsigned width(std::uint32_t point) { return point < 0x80 ? 1 : point < 0x800 ? 2 : point < 0x10000 ? 3 : 4; }
constexpr unsigned sequenceWidth(unsigned char lead) { return lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4; }

inline bool valid(const char* bytes, std::size_t size) {
    for (std::size_t index = 0; index < size;) {
        auto lead = static_cast<unsigned char>(bytes[index++]);
        if (lead < 0x80) continue;
        if (lead < 0xc2 || lead > 0xf4) return false;
        auto count = sequenceWidth(lead) - 1;
        if (size - index < count) return false;
        auto first = static_cast<unsigned char>(bytes[index]);
        if ((lead == 0xe0 && first < 0xa0) || (lead == 0xed && first >= 0xa0) ||
            (lead == 0xf0 && first < 0x90) || (lead == 0xf4 && first >= 0x90)) return false;
        for (unsigned item = 0; item < count; ++item) if ((static_cast<unsigned char>(bytes[index++]) & 0xc0) != 0x80) return false;
    }
    return true;
}

inline std::uint32_t decode(const char* bytes, unsigned size) {
    auto point = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[0]));
    if (size == 1) return point;
    point &= (1u << (7 - size)) - 1;
    for (unsigned index = 1; index < size; ++index) point = (point << 6) | (static_cast<unsigned char>(bytes[index]) & 0x3f);
    return point;
}

inline unsigned encode(std::uint32_t point, char* bytes) {
    auto size = width(point);
    for (auto index = size; index > 1; --index) {
        bytes[index - 1] = static_cast<char>(0x80 | (point & 0x3f));
        point >>= 6;
    }
    constexpr unsigned char prefix[] = {0, 0, 0xc0, 0xe0, 0xf0};
    bytes[0] = static_cast<char>(prefix[size] | point);
    return size;
}

}
