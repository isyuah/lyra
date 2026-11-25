#include "tasks.h"

#include <cstdint>

extern "C" uint8_t lyraHttpWatchMonitor(void* request, int (*watch)(uintptr_t, void*, int (*)(const void*)));

extern "C" uint8_t lyraHttpWatchDisconnect(void* request) {
    return lyraHttpWatchMonitor(request, lyraTaskWatchDisconnect);
}
