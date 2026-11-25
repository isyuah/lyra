#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace lyra {

struct ListSliceBounds { std::size_t start, count; };

inline std::optional<ListSliceBounds> listSliceBounds(std::size_t size, std::int64_t start, std::int64_t end, bool inclusive) {
    if (start < 0 || end < start) return {};
    auto limit = static_cast<std::uint64_t>(end) + inclusive;
    if (limit > size) return {};
    auto offset = static_cast<std::size_t>(start);
    return ListSliceBounds{offset, static_cast<std::size_t>(limit) - offset};
}

}
