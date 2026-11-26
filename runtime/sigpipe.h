#pragma once

#ifndef _WIN32
#include <cerrno>
#include <cstdlib>
#include <pthread.h>
#include <signal.h>
#endif

namespace lyraRuntime {

class SuppressSigpipe {
public:
#ifdef _WIN32
    SuppressSigpipe() = default;
#else
    SuppressSigpipe() {
        sigset_t pending;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGPIPE);
        if (pthread_sigmask(SIG_BLOCK, &blocked, &previous) || sigpending(&pending)) std::abort();
        alreadyPending = sigismember(&pending, SIGPIPE) == 1;
    }
    ~SuppressSigpipe() {
        if (!alreadyPending) {
            const auto saved = errno;
            constexpr timespec immediate{};
            while (sigtimedwait(&blocked, nullptr, &immediate) == -1 && errno == EINTR) {}
            errno = saved;
        }
        if (pthread_sigmask(SIG_SETMASK, &previous, nullptr)) std::abort();
    }
private:
    sigset_t blocked, previous;
    bool alreadyPending;
#endif
};

}
