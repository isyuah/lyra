#include <cinttypes>
#include <cstdio>
#include <cstdlib>

extern "C" void lyraPrintInt(std::int64_t value) {
    std::printf("%" PRId64 "\n", value);
}

extern "C" [[noreturn]] void lyraArithmeticError(const char* file, std::uint64_t line, std::uint64_t column, const char* reason) {
    std::fprintf(stderr, "%s:%" PRIu64 ":%" PRIu64 ": %s\n", file, line, column, reason);
    std::exit(EXIT_FAILURE);
}
