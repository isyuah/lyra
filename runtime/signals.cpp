#include "native_async.h"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#include <uv.h>

namespace {

std::mutex signalLock;

struct SignalWait {
    int number;
    std::int64_t* result;
    LyraNativeAsync* operation{};
    uv_async_t interrupt{};
    uv_signal_t watcher{};
    unsigned open{};
    bool closing{};

    static void closed(uv_handle_t* handle) {
        auto& wait = *static_cast<SignalWait*>(handle->data);
        if (--wait.open == 0) lyraNativeComplete(wait.operation, wait.result);
    }

    void close(int status) {
        std::lock_guard lock(signalLock);
        if (closing) return;
        closing = true;
        *result = status;
        if (open == 2) {
            uv_signal_stop(&watcher);
            uv_close(reinterpret_cast<uv_handle_t*>(&watcher), closed);
        }
        uv_close(reinterpret_cast<uv_handle_t*>(&interrupt), closed);
    }

    bool start(LyraNativeAsync* task) {
        if (number == 0) { *result = -1; return false; }
        operation = task;
        auto* loop = lyraNativeLoop(task);
        auto status = uv_async_init(loop, &interrupt, [](uv_async_t* handle) { static_cast<SignalWait*>(handle->data)->close(0); });
        if (status < 0) { *result = -status; return false; }
        interrupt.data = this;
        open = 1;
        status = uv_signal_init(loop, &watcher);
        if (!status) {
            watcher.data = this;
            open = 2;
            status = uv_signal_start(&watcher, [](uv_signal_t* signal, int) { static_cast<SignalWait*>(signal->data)->close(0); }, number);
        }
        if (status < 0) close(-status);
        return true;
    }

    void cancel() {
        std::lock_guard lock(signalLock);
        if (!closing && uv_async_send(&interrupt) != 0) std::abort();
    }
};

int signalNumber(std::int64_t kind) {
    if (kind == 0) return SIGINT;
#ifdef _WIN32
    if (kind == 2) return SIGBREAK;
#else
    if (kind == 1) return SIGTERM;
#endif
    return 0;
}

}

extern "C" LyraTask* lyraOsWaitSignal(std::int64_t kind) {
    auto* result = static_cast<std::int64_t*>(lyraGcAllocAtomic(sizeof(std::int64_t)));
    auto* wait = ::new (lyraGcAlloc(sizeof(SignalWait))) SignalWait{signalNumber(kind), result};
    return lyraTaskNative([](LyraNativeAsync* operation, void* data) { return static_cast<SignalWait*>(data)->start(operation); },
        [](void* data) { static_cast<SignalWait*>(data)->cancel(); }, wait, result);
}
