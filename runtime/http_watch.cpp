#include "tasks.h"

#include <cstdint>

extern "C" int lyraHttpDescriptor(const void* request);

extern "C" uint8_t lyraHttpWatchDisconnect(const void* request) {
    return lyraTaskWatchDisconnect(lyraHttpDescriptor(request)) != 0;
}
