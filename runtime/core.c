#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

void lyraPrintInt(int64_t value) {
    printf("%" PRId64 "\n", value);
}

_Noreturn void lyraIntOverflow(const char *file, uint64_t line, uint64_t column) {
    fprintf(stderr, "%s:%" PRIu64 ":%" PRIu64 ": Int overflow (+)\n", file, line, column);
    exit(EXIT_FAILURE);
}
