#include "io.h"

#include <cerrno>
#include <cstddef>
#include <cstring>

LyraIoText* lyraIoDecode(const char* data, std::size_t size, int status);

namespace {
int argumentCount;
char** arguments;
}

void lyraIoSetArgs(int argc, char** argv) {
    argumentCount = argc;
    arguments = argv;
}

int64_t lyraIoArgCount() {
    return argumentCount - 1;
}

LyraIoText* lyraIoArgument(int64_t index) {
    if (index < 0 || index >= lyraIoArgCount()) return lyraIoDecode(nullptr, 0, ERANGE);
    auto value = arguments[index + 1];
    return lyraIoDecode(value, std::strlen(value), 0);
}
