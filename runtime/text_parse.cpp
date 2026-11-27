#include "strings.h"

#include <charconv>
#include <cmath>

extern "C" int64_t lyraTextParseFloat(const void* text, double* output) {
    auto first = lyraStringBytes(text);
    auto last = first + lyraStringLength(text);
    if (first != last && *first == '+') {
        ++first;
        if (first == last || *first == '-' || *first == '+') return 1;
    }
    auto [end, error] = std::from_chars(first, last, *output, std::chars_format::general);
    if (end != last || error == std::errc::invalid_argument) return 1;
    if (error == std::errc::result_out_of_range) return 2;
    return std::isfinite(*output) ? 0 : 1;
}
