#pragma once
#include <cstdint>
#include <string_view>

namespace lyra {
inline constexpr std::uint64_t valueHashSeed = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t valueHashFactor = 0x100000001b3ULL;

constexpr std::uint64_t mixValueHash(std::uint64_t state, std::uint64_t part) {
    auto mixed = (state ^ part) * valueHashFactor;
    return mixed ^ (mixed >> 32);
}

constexpr std::uint64_t hashValueBytes(std::string_view bytes) {
    auto hash = valueHashSeed;
    for (unsigned char byte : bytes) hash = mixValueHash(hash, byte);
    return hash;
}
}
