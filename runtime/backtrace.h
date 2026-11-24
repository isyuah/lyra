#pragma once

#include <cstdio>

namespace lyraRuntime {

struct Backtrace;
const Backtrace* captureBacktrace();
void writeBacktrace(const Backtrace& trace, FILE* stream);

}
