#include <cinttypes>
#include <cstdio>
#include <cstdlib>

extern "C" void lyraPrintInt(std::int64_t value) {
    std::printf("%" PRId64 "\n", value);
}

extern "C" [[noreturn]] void lyraIntOverflow(const char* file, std::uint64_t line, std::uint64_t column) {
    std::fprintf(stderr, "%s:%" PRIu64 ":%" PRIu64 ": Int overflow (+)\n", file, line, column);
    std::exit(EXIT_FAILURE);
}
