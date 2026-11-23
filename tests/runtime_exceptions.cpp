#include "exceptions.h"
#include "managed.h"
#include "strings.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void check(bool value, const char* reason) {
    if (!value) { std::fprintf(stderr, "exceptions: %s\n", reason); std::exit(EXIT_FAILURE); }
}

const LyraException* raised(const char* message, std::uint64_t line) {
    return lyraExceptionRaise(lyraExceptionCreate(lyraStringFromBytes(message, std::strlen(message))), "source.ly", line, 4);
}

std::string report(const LyraException* failure) {
#ifdef _WIN32
    FILE* file;
    check(tmpfile_s(&file) == 0, "cannot create report stream");
#else
    FILE* file = std::tmpfile();
    check(file != nullptr, "cannot create report stream");
#endif
    lyraExceptionWrite(failure, file);
    std::rewind(file);
    std::string result;
    char block[256];
    while (auto count = std::fread(block, 1, sizeof(block), file)) result.append(block, count);
    std::fclose(file);
    return result;
}

}

int main() {
    auto primary = raised("business", 8);
    auto first = raised("cleanup-new", 21), second = raised("cleanup-old", 32);
    auto trace = lyraExceptionFrame(primary, "await", "awaiting.ly", 15, 2);
    auto after = lyraExceptionSuppress(lyraExceptionSuppress(trace, first), second);
    lyraGcCollect();
    check(lyraExceptionSuppressedCount(primary) == 0 && lyraExceptionSuppressedCount(after) == 2, "another observer's failure was mutated");
    check(lyraExceptionRaise(after, "rethrown.ly", 66, 1) == after, "rethrow replaced the original throw site");
    check(report(after) == "Exception: business\n  throw at source.ly:8:4\n  await at awaiting.ly:15:2\n"
        "suppressed Exception: cleanup-new\n  throw at source.ly:21:4\nsuppressed Exception: cleanup-old\n  throw at source.ly:32:4\n",
        "exception report lost the original site, await frame or ordered cleanup failures");
    auto other = lyraExceptionFrame(primary, "await", "other.ly", 9, 3);
    check(report(other).find("awaiting.ly") == std::string::npos && report(other).find("other.ly:9:3") != std::string::npos,
        "independent waiters shared an await frame");
    const std::string binary("left\0right", 10);
    check(report(lyraExceptionCreate(lyraStringFromBytes(binary.data(), binary.size()))).find(binary) != std::string::npos,
        "embedded NUL truncated a Lyra message");
    check(lyraExceptionSuppress(nullptr, first) == first, "a failing cleanup after normal exit did not become primary");
}
