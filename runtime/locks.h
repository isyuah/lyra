#pragma once

#include <cstdlib>
#include <uv.h>

namespace lyraRuntime {

inline void initMutex(uv_mutex_t& mutex) {
    if (uv_mutex_init(&mutex)) std::abort();
}

class Lock {
public:
    explicit Lock(uv_mutex_t& mutex) : mutex(mutex) { uv_mutex_lock(&mutex); }
    ~Lock() { uv_mutex_unlock(&mutex); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    uv_mutex_t& mutex;
};

}
