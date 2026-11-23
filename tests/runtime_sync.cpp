#include "managed.h"
#include "strings.h"
#include "sync.h"
#include "tasks.h"

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <stdio.h>
#include <thread>

namespace {

constexpr int64_t updates = 4096;

void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "sync test: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

struct Shared {
    LyraMutex *mutex;
    LyraAtomicInt *atomic;
    const void *token{};
    int64_t count{};
};

struct SyncCase;

struct Peer {
    SyncCase *test;
    unsigned index;
    int64_t sum{};
};

struct SyncCase {
    Shared *shared;
    Peer peers[2];
    std::mutex gate;
    std::condition_variable changed;
    std::thread::id threads[2];
    bool tokenPublished{}, collected{};
};

[[gnu::noinline]] void publishToken(Shared& shared) {
    lyraMutexLock(shared.mutex);
    shared.token = lyraStringFromBytes("shared root", 11);
    lyraMutexUnlock(shared.mutex);
}

void collectDuringCriticalSection(Peer& peer) {
    auto& test = *peer.test;
    auto& shared = *test.shared;
    if (peer.index == 0) {
        lyraMutexLock(shared.mutex);
        publishToken(shared);
        const void *local = lyraStringFromBytes("worker root", 11);
        {
            std::unique_lock gate(test.gate);
            test.tokenPublished = true;
            test.changed.notify_all();
            test.changed.wait(gate, [&] { return test.collected; });
        }
        check(lyraStringLength(local) == 11 && !memcmp(lyraStringBytes(local), "worker root", 11),
              "a token held only on a registered worker stack was lost");
        lyraMutexUnlock(shared.mutex);
    } else {
        {
            std::unique_lock gate(test.gate);
            test.changed.wait(gate, [&] { return test.tokenPublished; });
        }
        for (unsigned round = 0; round < 3; ++round) {
            lyraGcCollect();
            for (unsigned index = 0; index < 2048; ++index) lyraStringFromBytes("other value", 11);
        }
        {
            std::lock_guard gate(test.gate);
            test.collected = true;
            test.changed.notify_all();
        }
        lyraMutexLock(shared.mutex);
        check(lyraStringLength(shared.token) == 11 && !memcmp(lyraStringBytes(shared.token), "shared root", 11),
              "a reference held under a mutex was lost during cross-thread collection");
        lyraMutexUnlock(shared.mutex);
    }
}

void update(LyraTask *task, void *storage) {
    auto& peer = *static_cast<Peer *>(storage);
    auto& test = *peer.test;
    {
        std::unique_lock gate(test.gate);
        test.threads[peer.index] = std::this_thread::get_id();
        test.changed.notify_all();
        test.changed.wait(gate, [&] { return test.threads[1 - peer.index] != std::thread::id{}; });
        check(test.threads[0] != test.threads[1], "test never ran in two concurrent OS threads");
    }
    collectDuringCriticalSection(peer);
    auto& shared = *test.shared;
    for (int64_t index = 0; index < updates; ++index) {
        lyraMutexLock(shared.mutex);
        ++shared.count;
        lyraMutexUnlock(shared.mutex);
        peer.sum += lyraAtomicIntFetchAdd(shared.atomic, 1);
    }
    lyraTaskComplete(task, nullptr);
}

void runBoth(LyraTask *task, void *storage) {
    auto& test = *static_cast<SyncCase *>(storage);
    for (auto& peer : test.peers) lyraTaskParallel(task, lyraTaskCreate(update, &peer));
    lyraTaskComplete(task, nullptr);
}

}

int main() {
    auto *shared = ::new (lyraGcAlloc(sizeof(Shared))) Shared{lyraMutexCreate(), lyraAtomicIntCreate(0)};
    SyncCase test{shared, {{&test, 0}, {&test, 1}}};
    auto *root = lyraTaskCreate(runBoth, &test);
    check(lyraTaskRunRoot(root) == 0 && lyraTaskState(root) == LYRA_TASK_COMPLETED, "parallel updates did not finish");
    constexpr auto total = 2 * updates;
    check(shared->count == total, "mutex lost a protected update");
    check(lyraAtomicIntLoad(shared->atomic) == total && test.peers[0].sum + test.peers[1].sum == total * (total - 1) / 2,
          "fetchAdd did not return the preceding values of a single counter");
    check(!lyraAtomicIntCompareExchange(shared->atomic, total - 1, 0) && lyraAtomicIntLoad(shared->atomic) == total,
          "compareExchange wrote despite a mismatch");
    check(lyraAtomicIntCompareExchange(shared->atomic, total, -32) && lyraAtomicIntLoad(shared->atomic) == -32,
          "compareExchange did not store the replacement");
    lyraAtomicIntStore(shared->atomic, INT64_MAX);
    check(lyraAtomicIntFetchAdd(shared->atomic, 1) == INT64_MAX && lyraAtomicIntLoad(shared->atomic) == INT64_MIN,
          "fetchAdd did not wrap in two's complement");
}
