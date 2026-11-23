#include "tasks.h"

#include <cstdint>

extern "C" uintptr_t lyraHttpDisconnectMonitor(const void* request);

extern "C" uint8_t lyraHttpWatchDisconnect(const void* request) {
    return lyraTaskWatchDisconnect(lyraHttpDisconnectMonitor(request)) != 0;
}
