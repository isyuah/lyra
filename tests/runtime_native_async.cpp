#include "native_async.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {

void check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "native async test: %s\n", message);
    std::abort();
}

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    LyraNativeAsync* operation{};
    bool cancelled{};

    LyraTask* task() {
        return lyraTaskNative([](LyraNativeAsync* operation, void* data) {
            auto& gate = *static_cast<Gate*>(data);
            std::lock_guard lock(gate.mutex);
            gate.operation = operation;
            gate.changed.notify_one();
            return true;
        }, [](void* data) {
            auto& gate = *static_cast<Gate*>(data);
            std::lock_guard lock(gate.mutex);
            gate.cancelled = true;
        }, this);
    }

    LyraNativeAsync* wait() {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return operation != nullptr; });
        return operation;
    }
};

void awaitAcquisition(LyraTask* self, void* storage) {
    const LyraException* error;
    if (lyraTaskCancelRequested(self)) lyraTaskFinishCancelled(self);
    else if (lyraTaskAwaitValue(self, static_cast<LyraTask*>(storage), &error) == LYRA_AWAIT_VALUE)
        lyraTaskComplete(self, nullptr);
}

}

int main() {
    auto* domain = lyraDomainCreate();
    Gate cold;
    auto* unused = cold.task();
    lyraTaskRequestCancel(unused);
    lyraTaskStart(domain, unused);
    lyraDomainRun(domain);
    check(!cold.operation && !cold.cancelled, "a cancelled cold task touched native state");

    Gate pending;
    auto* cancelled = pending.task();
    lyraTaskStart(domain, cancelled);
    std::thread cancel([&] {
        auto registration = lyraGcEnterWorker();
        auto* operation = pending.wait();
        lyraTaskRequestCancel(cancelled);
        check(pending.cancelled, "native operation missed cancellation");
        lyraTaskWake(cancelled);
        check(lyraTaskState(cancelled) == LYRA_TASK_SUSPENDED, "cancel resumed before native acknowledgement");
        check(!lyraNativeTryComplete(operation, nullptr), "cancelled acquisition published a result");
        lyraGcCollect();
        lyraNativeComplete(operation);
        lyraGcLeaveWorker(registration);
    });
    lyraDomainRun(domain);
    cancel.join();
    check(lyraTaskState(cancelled) == LYRA_TASK_CANCELLED, "native acknowledgement did not resume its domain");

    Gate acquire, release;
    auto* acquisition = acquire.task();
    auto* owner = lyraTaskCreate(awaitAcquisition, acquisition);
    lyraTaskStart(domain, owner);
    std::thread complete([&] {
        auto registration = lyraGcEnterWorker();
        auto* operation = acquire.wait();
        check(lyraNativeTryComplete(operation, nullptr, release.task()), "acquisition could not register a release");
        auto* cleanup = release.wait();
        lyraTaskRequestCancel(owner);
        check(lyraTaskState(owner) == LYRA_TASK_SUSPENDED && !release.cancelled, "owner abandoned a pending cleanup");
        lyraGcCollect();
        lyraNativeComplete(cleanup);
        lyraGcLeaveWorker(registration);
    });
    lyraDomainRun(domain);
    complete.join();
    check(lyraTaskState(acquisition) == LYRA_TASK_COMPLETED && lyraTaskState(owner) == LYRA_TASK_CANCELLED,
        "completion winner or cleanup-dependent terminal state was lost");
    lyraDomainDestroy(domain);
}
