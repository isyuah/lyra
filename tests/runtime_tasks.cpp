#include "context.h"
#include "lists.h"
#include "managed.h"
#include "net.h"
#include "strings.h"
#include "tasks.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>

namespace {

void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "task test: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

void checkFailures(LyraTask* task, const LyraException* first, const LyraException* second) {
    check(lyraTaskState(task) == LYRA_TASK_FAILED, "task group did not fail");
    auto* failures = lyraExceptionTaskFailures(lyraTaskException(task));
    check(lyraListSize(failures) == 2, "task group lost a child failure");
    auto at = [&](int index) { return *static_cast<const LyraException* const*>(lyraListGet(failures, index, __FILE__, __LINE__, 1)); };
    check(at(0) == first && at(1) == second, "task group failures were not in registration order");
}

struct Counter {
    unsigned count;
};

struct ProducerFrame {
    Counter *captured;
    unsigned *executions;
    bool shouldFail;
};

void produce(LyraTask *self, void *storage) {
    auto *frame = static_cast<ProducerFrame *>(storage);
    if (!frame->captured) {
        ++*frame->executions;
        frame->captured = static_cast<Counter *>(lyraGcAlloc(sizeof(Counter)));
        frame->captured->count = 173;
        lyraTaskSuspend(self);
    } else if (frame->shouldFail) {
        lyraTaskFail(self, lyraExceptionCreate(lyraStringFromBytes("producer", 8)));
    } else {
        lyraTaskComplete(self, frame->captured);
    }
}

LyraTask *producer(unsigned *executions, bool shouldFail) {
    auto *frame = static_cast<ProducerFrame *>(lyraGcAlloc(sizeof(ProducerFrame)));
    frame->executions = executions;
    frame->shouldFail = shouldFail;
    return lyraTaskCreate(produce, frame);
}

struct WaitFrame {
    LyraTask *target;
    unsigned *cleanups;
};

void observe(LyraTask *self, void *storage) {
    auto *frame = static_cast<WaitFrame *>(storage);
    if (lyraTaskCancelRequested(self)) {
        ++*frame->cleanups;
        lyraTaskFinishCancelled(self);
        return;
    }
    if (!lyraTaskAwait(self, frame->target)) return;
    if (lyraTaskState(frame->target) == LYRA_TASK_FAILED) {
        lyraTaskFail(self, lyraTaskException(frame->target));
        return;
    }
    check(lyraTaskState(frame->target) == LYRA_TASK_COMPLETED, "unexpected observed outcome");
    lyraTaskComplete(self, lyraTaskOutcome(frame->target));
}

LyraTask *observer(LyraTask *target, unsigned *cleanups) {
    auto *frame = static_cast<WaitFrame *>(lyraGcAlloc(sizeof(WaitFrame)));
    frame->target = target;
    frame->cleanups = cleanups;
    return lyraTaskCreate(observe, frame);
}

struct SpawnFrame {
    LyraDomain *domain;
    LyraTask *sibling;
    unsigned *counter;
};

void increment(LyraTask *self, void *storage) {
    ++*static_cast<unsigned *>(storage);
    lyraTaskComplete(self, nullptr);
}

void spawnThenIncrement(LyraTask *self, void *storage) {
    auto *frame = static_cast<SpawnFrame *>(storage);
    auto before = *frame->counter;
    lyraTaskStart(frame->domain, frame->sibling);
    check(*frame->counter == before, "spawn interleaved inside a non-suspending step");
    *frame->counter = before + 1;
    lyraTaskComplete(self, nullptr);
}

void collectUnderPressure() {
    for (int round = 0; round < 3; ++round) {
        for (int index = 0; index < 2048; ++index) lyraGcAlloc(256);
        lyraGcCollect();
    }
}

struct NativeTicket {
    LyraTask *task;
    unsigned observed;
};

struct ParkedFrame {
    NativeTicket *ticket;
    Counter *soleReference;
};

void park(LyraTask *self, void *storage) {
    auto *frame = static_cast<ParkedFrame *>(storage);
    if (!frame->soleReference) {
        frame->soleReference = static_cast<Counter *>(lyraGcAlloc(sizeof(Counter)));
        frame->soleReference->count = 794;
        lyraTaskSuspend(self);
    } else {
        frame->ticket->observed = frame->soleReference->count;
        lyraTaskComplete(self, nullptr);
    }
}

[[gnu::noinline]] void handOff(LyraDomain *domain, NativeTicket *ticket) {
    auto *frame = static_cast<ParkedFrame *>(lyraGcAlloc(sizeof(ParkedFrame)));
    frame->ticket = ticket;
    ticket->task = lyraTaskCreate(park, frame);
    lyraTaskStart(domain, ticket->task);
}

struct NativeGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool mayReturn = false;
    bool returned = false;
    bool closed = false;
};

void nativeCall(void *storage) {
    auto *gate = static_cast<NativeGate *>(storage);
    std::unique_lock lock(gate->mutex);
    gate->entered = true;
    gate->changed.notify_one();
    gate->changed.wait(lock, [&] { return gate->mayReturn; });
    check(!gate->closed, "cancel freed a handle still in use by a worker");
    gate->returned = true;
}

struct BlockingFrame {
    NativeGate *gate;
    bool submitted;
};

void callThenClose(LyraTask *self, void *storage) {
    auto *frame = static_cast<BlockingFrame *>(storage);
    if (!frame->submitted) {
        frame->submitted = true;
        lyraTaskBlock(self, nativeCall, frame->gate);
        return;
    }
    check(lyraTaskCancelRequested(self), "native cancellation request was lost");
    check(frame->gate->returned, "cleanup ran before the native worker returned");
    frame->gate->closed = true;
    lyraTaskFinishCancelled(self);
}

struct CancellationFrame {
    LyraTask *target;
    NativeGate *gate;
};

void cancelDuringCall(LyraTask *self, void *storage) {
    auto *frame = static_cast<CancellationFrame *>(storage);
    auto *gate = frame->gate;
    std::unique_lock lock(gate->mutex);
    gate->changed.wait(lock, [&] { return gate->entered; });
    lyraTaskRequestCancel(frame->target);
    lyraTaskWake(frame->target);
    check(lyraTaskState(frame->target) == LYRA_TASK_SUSPENDED && !gate->closed, "native request woke before completion");
    lyraGcCollect();
    gate->mayReturn = true;
    gate->changed.notify_one();
    lock.unlock();
    lyraTaskComplete(self, nullptr);
}

struct DisconnectCase {
    lyraRuntime::net::Socket sockets[2];
    uintptr_t monitor{};
    bool *locallyClosed{};
    NativeGate gate;
    LyraTask *handler{}, *blocked{};
    LyraCleanup *cleanups{};
    bool observed{}, closed{};
};

int disconnectSocketClosed(const void *target) {
    return *static_cast<const bool *>(target);
}

void *queuedClosure(void *storage, const LyraException **) {
    auto *test = static_cast<DisconnectCase *>(storage);
    check(!lyraTaskWatchDisconnect(test->monitor, test->locallyClosed, disconnectSocketClosed), "worker shared the caller's current task");
    lyraRuntime::net::close(test->sockets[1]);
    nativeCall(&test->gate);
    lyraGcCollect();
    return lyraGcAlloc(sizeof(Counter));
}

void observeDisconnect(LyraTask *self, void *storage) {
    auto *test = static_cast<DisconnectCase *>(storage);
    if (!lyraTaskCancelRequested(test->handler)) {
        lyraTaskSuspend(self);
        lyraTaskWake(self);
        return;
    }
    std::unique_lock lock(test->gate.mutex);
    check(!test->closed && lyraTaskCancelRequested(test->blocked), "disconnect did not cancel the worker's owner tree");
    check(lyraTaskState(test->handler) == LYRA_TASK_SUSPENDED && lyraTaskState(test->blocked) == LYRA_TASK_SUSPENDED,
          "pending C operation or its parent resumed before the worker returned");
    test->observed = true;
    test->gate.mayReturn = true;
    test->gate.changed.notify_one();
    lock.unlock();
    lyraTaskComplete(self, nullptr);
}

void watchedHandler(LyraTask *self, void *storage) {
    auto *test = static_cast<DisconnectCase *>(storage);
    if (!test->blocked) {
        lyraCleanupPush(&test->cleanups, [](void *environment) -> const LyraException * {
            auto *closing = static_cast<DisconnectCase *>(environment);
            check(closing->gate.returned && closing->observed && !closing->closed, "defer closed a resource before opaque native work ended");
            *closing->locallyClosed = true;
            lyraRuntime::net::close(closing->sockets[0]);
            lyraRuntime::net::closeMonitor(closing->monitor);
            closing->closed = true;
            return nullptr;
        }, test);
        check(lyraTaskWatchDisconnect(test->monitor, test->locallyClosed, disconnectSocketClosed), "owner could not watch a socket");
        test->blocked = lyraTaskBlocking(queuedClosure, test);
        check(!lyraTaskAwait(self, test->blocked), "cold blocking closure was already done");
        return;
    }
    check(lyraTaskCancelRequested(self) && test->observed && test->gate.returned, "cleanup ran early or missed disconnect");
    check(lyraTaskState(test->blocked) == LYRA_TASK_CANCELLED, "blocking work did not complete cancellation");
    lyraTaskFinishCancelled(self);
}

struct GroupCase {
    LyraTask *early;
    unsigned order;
    const LyraException *causes[2];
    LyraCleanup *cleanups;
    unsigned cleaned;
};

void firstFailure(LyraTask *self, void *storage) {
    auto *test = static_cast<GroupCase *>(storage);
    if (!lyraTaskCancelRequested(self)) { lyraTaskSuspend(self); return; }
    check(++test->order == 2, "unexpected failure order");
    lyraTaskFail(self, test->causes[0]);
}

void secondFailure(LyraTask *self, void *storage) {
    auto *test = static_cast<GroupCase *>(storage);
    check(++test->order == 1, "second child did not fail first");
    lyraTaskFail(self, test->causes[1]);
}

void failGroup(LyraTask *self, void *storage) {
    auto *test = static_cast<GroupCase *>(storage);
    lyraCleanupPush(&test->cleanups, [](void *environment) -> const LyraException * {
        auto *closing = static_cast<GroupCase *>(environment);
        check(closing->order == 2, "failure cleanup did not wait for the other child");
        ++closing->cleaned;
        return nullptr;
    }, test);
    test->early = lyraTaskSpawn(self, lyraTaskCreate(firstFailure, test));
    lyraTaskSpawn(self, lyraTaskCreate(secondFailure, test));
    lyraTaskComplete(self, nullptr);
    check(lyraTaskState(self) == LYRA_TASK_SUSPENDED, "group completed before children finished");
}

struct ParallelCase;

struct ParallelPeer {
    ParallelCase *test;
    unsigned index;
};

struct ParallelCase {
    std::mutex mutex;
    std::condition_variable changed;
    std::thread::id caller, peers[2];
    ParallelPeer frames[2];
    LyraTask *children[2]{};
    unsigned arrived{}, sameDomainSteps{}, initialLocal{}, laterLocal{};
    bool mayComplete{}, resumed{};
};

void rendezvous(LyraTask *self, void *storage) {
    auto *peer = static_cast<ParallelPeer *>(storage);
    auto& test = *peer->test;
    auto *answer = static_cast<Counter *>(lyraGcAlloc(sizeof(Counter)));
    answer->count = 90 + peer->index;
    check(lyraContextLocal(lyraContextCurrent(), &test) == &test.initialLocal, "parallel task did not capture its activation context");
    {
        std::unique_lock lock(test.mutex);
        test.peers[peer->index] = std::this_thread::get_id();
        ++test.arrived;
        test.changed.notify_all();
        test.changed.wait(lock, [&] { return test.mayComplete; });
    }
    lyraTaskComplete(self, answer);
}

void openRendezvous(LyraTask *self, void *storage) {
    auto& test = *static_cast<ParallelCase *>(storage);
    check(std::this_thread::get_id() == test.caller, "spawn left its caller's domain");
    ++test.sameDomainSteps;
    {
        std::lock_guard lock(test.mutex);
        test.mayComplete = true;
        test.changed.notify_all();
    }
    lyraTaskComplete(self, nullptr);
}

void parallelRendezvous(LyraTask *self, void *storage) {
    auto& test = *static_cast<ParallelCase *>(storage);
    if (!test.children[0]) {
        test.caller = std::this_thread::get_id();
        check(lyraContextEnter(lyraContextWithLocal(lyraContextCurrent(), &test, &test.initialLocal)), "cannot install activation context");
        for (unsigned index = 0; index < 2; ++index) {
            test.frames[index] = {&test, index};
            test.children[index] = lyraTaskParallel(self, lyraTaskCreate(rendezvous, &test.frames[index]));
        }
        check(lyraContextEnter(lyraContextWithLocal(lyraContextCurrent(), &test, &test.laterLocal)), "cannot replace parent context");
        lyraTaskSpawn(self, lyraTaskCreate(openRendezvous, &test));
        {
            std::unique_lock lock(test.mutex);
            test.changed.wait(lock, [&] { return test.arrived == 2; });
            check(test.peers[0] != test.peers[1] && test.peers[0] != test.caller && test.peers[1] != test.caller,
                  "parallel domains did not reach the same gate on distinct OS threads");
        }
        check(!test.sameDomainSteps, "spawn ran during an unsuspended step in its own domain");
        collectUnderPressure();
        check(lyraTaskAwaitValue(self, test.children[0]) == LYRA_AWAIT_PENDING, "cross-domain await did not park on a closed gate");
        return;
    }
    check(std::this_thread::get_id() == test.caller && test.sameDomainSteps == 1, "parallel completion resumed outside the waiter's domain");
    check(lyraContextLocal(lyraContextCurrent(), &test) == &test.laterLocal, "cross-domain resume changed the waiter's context");
    for (unsigned index = 0; index < 2; ++index) {
        auto *target = test.children[index];
        if (lyraTaskAwaitValue(self, target) == LYRA_AWAIT_PENDING) return;
        check(lyraTaskState(target) == LYRA_TASK_COMPLETED && static_cast<Counter *>(lyraTaskOutcome(target))->count == 90 + index,
              "parallel result was lost during collection or transfer");
    }
    test.resumed = true;
    lyraTaskComplete(self, nullptr);
}

struct NestedDomain {
    unsigned depth;
    LyraTask *child;
};

void nestedDomain(LyraTask *self, void *storage) {
    auto& frame = *static_cast<NestedDomain *>(storage);
    if (frame.depth) {
        if (!frame.child) {
            auto *child = static_cast<NestedDomain *>(lyraGcAlloc(sizeof(NestedDomain)));
            *child = {frame.depth - 1, nullptr};
            frame.child = lyraTaskParallel(self, lyraTaskCreate(nestedDomain, child));
        }
        if (lyraTaskAwaitValue(self, frame.child) == LYRA_AWAIT_PENDING) return;
        check(lyraTaskState(frame.child) == LYRA_TASK_COMPLETED, "nested domain failed while sharing the worker pool");
    }
    lyraTaskComplete(self, nullptr);
}

struct StealCase {
    std::mutex mutex;
    std::condition_variable changed;
    std::thread::id occupiedWorker;
    unsigned completed{};
};

void occupyWorker(LyraTask *self, void *storage) {
    auto& test = *static_cast<StealCase *>(storage);
    lyraTaskSpawn(self, lyraTaskCreate([](LyraTask *child, void *storage) {
        auto& test = *static_cast<StealCase *>(storage);
        std::lock_guard lock(test.mutex);
        check(test.completed == 2, "the same execution domain ran while its parent step was still active");
        lyraTaskComplete(child, nullptr);
    }, storage));
    {
        std::unique_lock lock(test.mutex);
        test.occupiedWorker = std::this_thread::get_id();
        test.changed.notify_all();
        check(test.changed.wait_for(lock, std::chrono::seconds(10), [&] { return test.completed == 2; }),
              "runnable domains did not make progress while another Worker was occupied");
    }
    lyraTaskComplete(self, nullptr);
}

void scheduleSteal(LyraTask *self, void *storage) {
    auto& test = *static_cast<StealCase *>(storage);
    lyraTaskParallel(self, lyraTaskCreate(occupyWorker, storage));
    {
        std::unique_lock lock(test.mutex);
        check(test.changed.wait_for(lock, std::chrono::seconds(10), [&] { return test.occupiedWorker != std::thread::id{}; }),
              "parallel Worker did not reach its gate");
    }
    for (unsigned index = 0; index < 2; ++index) lyraTaskParallel(self, lyraTaskCreate([](LyraTask *child, void *storage) {
        auto& test = *static_cast<StealCase *>(storage);
        auto *value = static_cast<Counter *>(lyraGcAlloc(sizeof(Counter)));
        value->count = 73;
        collectUnderPressure();
        check(value->count == 73, "a managed value was lost on a Worker during stealing");
        {
            std::lock_guard lock(test.mutex);
            check(std::this_thread::get_id() != test.occupiedWorker, "independent runnable work remained on the occupied Worker");
            ++test.completed;
            test.changed.notify_all();
        }
        lyraTaskComplete(child, nullptr);
    }, storage));
    lyraTaskComplete(self, nullptr);
}

struct ParallelNative {
    NativeGate gate;
    BlockingFrame work{&gate, false};
    LyraTask *owner{}, *child{};
    LyraCleanup *cleanups{};
    std::thread::id caller;
    bool cleaned{};
};

void joinNative(LyraTask *self, void *storage) {
    auto& test = *static_cast<ParallelNative *>(storage);
    test.caller = std::this_thread::get_id();
    lyraCleanupPush(&test.cleanups, [](void *storage) -> const LyraException * {
        auto& test = *static_cast<ParallelNative *>(storage);
        check(std::this_thread::get_id() == test.caller, "joining cleanup ran on the child's domain");
        check(test.gate.returned && test.gate.closed, "parent closed before a parallel native handle was safe");
        test.cleaned = true;
        return nullptr;
    }, &test);
    test.child = lyraTaskParallel(self, lyraTaskCreate(callThenClose, &test.work));
    {
        std::unique_lock lock(test.gate.mutex);
        test.gate.changed.wait(lock, [&] { return test.gate.entered; });
        lyraTaskRequestCancel(self);
        check(lyraTaskCancelRequested(test.child), "cancellation did not reach the parallel native task");
    }
    lyraTaskFinishCancelled(self);
}

void releaseNative(LyraTask *self, void *storage) {
    auto& test = *static_cast<ParallelNative *>(storage);
    {
        std::lock_guard lock(test.gate.mutex);
        check(lyraTaskState(test.owner) == LYRA_TASK_SUSPENDED && lyraTaskState(test.child) == LYRA_TASK_SUSPENDED && !test.cleaned,
              "cancellation published before a cross-domain native operation returned");
        test.gate.mayReturn = true;
        test.gate.changed.notify_all();
    }
    lyraTaskComplete(self, nullptr);
}

struct ParallelFailure {
    std::mutex mutex;
    std::condition_variable changed;
    const LyraException *causes[2];
    bool parked{};
};

void waitForFailure(LyraTask *self, void *storage) {
    auto& test = *static_cast<ParallelFailure *>(storage);
    if (lyraTaskCancelRequested(self)) {
        lyraTaskFail(self, test.causes[0]);
        return;
    }
    std::lock_guard lock(test.mutex);
    lyraTaskSuspend(self);
    test.parked = true;
    test.changed.notify_all();
}

void publishFailure(LyraTask *self, void *storage) {
    auto& test = *static_cast<ParallelFailure *>(storage);
    {
        std::unique_lock lock(test.mutex);
        test.changed.wait(lock, [&] { return test.parked; });
    }
    lyraTaskFail(self, test.causes[1]);
}

void parallelFailureGroup(LyraTask *self, void *storage) {
    lyraTaskParallel(self, lyraTaskCreate(waitForFailure, storage));
    lyraTaskParallel(self, lyraTaskCreate(publishFailure, storage));
    lyraTaskComplete(self, nullptr);
}

void checkParallel(LyraDomain *origin) {
    ParallelCase rendezvous{};
    auto *parent = lyraTaskCreate(parallelRendezvous, &rendezvous);
    lyraTaskStart(origin, parent);
    lyraDomainRun(origin);
    check(rendezvous.resumed && lyraTaskState(parent) == LYRA_TASK_COMPLETED, "parallel rendezvous did not finish");

    auto *deep = lyraTaskCreate([](LyraTask *self, void *) {
        auto *frame = static_cast<NestedDomain *>(lyraGcAlloc(sizeof(NestedDomain)));
        *frame = {8, nullptr};
        lyraTaskParallel(self, lyraTaskCreate(nestedDomain, frame));
        lyraTaskComplete(self, nullptr);
    }, nullptr);
    lyraTaskStart(origin, deep);
    lyraDomainRun(origin);
    check(lyraTaskState(deep) == LYRA_TASK_COMPLETED, "nested awaits consumed all available workers");

    StealCase steal;
    auto *balanced = lyraTaskCreate(scheduleSteal, &steal);
    lyraTaskStart(origin, balanced);
    lyraDomainRun(origin);
    check(lyraTaskState(balanced) == LYRA_TASK_COMPLETED && steal.completed == 2, "stealing lost a ready domain");

    ParallelNative native;
    native.owner = lyraTaskCreate(joinNative, &native);
    lyraTaskKeepCleanups(native.owner, &native.cleanups);
    lyraTaskStart(origin, native.owner);
    lyraTaskStart(origin, lyraTaskCreate(releaseNative, &native));
    lyraDomainRun(origin);
    check(native.cleaned && lyraTaskState(native.owner) == LYRA_TASK_CANCELLED && lyraTaskState(native.child) == LYRA_TASK_CANCELLED,
          "parallel native cancellation did not complete cleanup");

    ParallelFailure failure{};
    failure.causes[0] = lyraExceptionCreate(lyraStringFromBytes("first", 5));
    failure.causes[1] = lyraExceptionCreate(lyraStringFromBytes("second", 6));
    auto *failed = lyraTaskCreate(parallelFailureGroup, &failure);
    lyraTaskStart(origin, failed);
    lyraDomainRun(origin);
    checkFailures(failed, failure.causes[0], failure.causes[1]);
}

void checkChannels(LyraDomain* origin, LyraDomain* other) {
    int initial{11}, removed{22}, last{33};
    unsigned closedType{};
    const LyraException* error{};
    auto* buffered = lyraChannelCreate(1, &closedType, &error);
    auto receive = [](LyraChannel* channel) { return lyraChannelReceive(channel, [](void* value) { return value; }); };
    auto start = [](LyraDomain* domain, LyraTask* task) { lyraTaskStart(domain, task); return task; };
    start(origin, lyraChannelSend(buffered, &initial));
    auto* abandoned = start(origin, lyraChannelSend(buffered, &removed));
    auto* waiting = start(origin, lyraChannelSend(buffered, &last));
    lyraDomainRun(origin);
    check(lyraTaskState(abandoned) == LYRA_TASK_SUSPENDED && lyraTaskState(waiting) == LYRA_TASK_SUSPENDED,
        "full channel did not apply backpressure");
    lyraTaskRequestCancel(abandoned);
    auto* first = start(other, receive(buffered));
    lyraDomainRun(other);
    lyraDomainRun(origin);
    check(lyraTaskState(abandoned) == LYRA_TASK_CANCELLED && lyraTaskState(waiting) == LYRA_TASK_COMPLETED && lyraTaskOutcome(first) == &initial,
        "cancelling a sender lost a buffered value or admitted the wrong waiting sender");
    lyraChannelClose(buffered);
    lyraChannelClose(buffered);
    auto* second = start(origin, receive(buffered));
    auto* empty = start(origin, receive(buffered));
    auto* rejected = start(origin, lyraChannelSend(buffered, &removed));
    lyraDomainRun(origin);
    check(lyraTaskOutcome(second) == &last && lyraTaskOutcome(empty) == nullptr, "closed channel did not drain in FIFO order");
    check(lyraTaskState(rejected) == LYRA_TASK_FAILED && lyraExceptionType(lyraTaskException(rejected)) == &closedType,
        "sending to a closed channel did not report the typed exception");

    auto* firstChannel = lyraChannelCreate(0, &closedType, &error);
    auto* secondChannel = lyraChannelCreate(0, &closedType, &error);
    auto selectReceive = [&] {
        auto* choose = lyraTaskSelect(2);
        lyraTaskSelectArm(choose, 0, receive(firstChannel));
        lyraTaskSelectArm(choose, 1, receive(secondChannel));
        return start(origin, choose);
    };
    auto* cancelled = selectReceive();
    lyraDomainRun(origin);
    check(lyraTaskState(cancelled) == LYRA_TASK_SUSPENDED, "select did not wait for a matching message");
    lyraTaskRequestCancel(cancelled);
    auto* committed = selectReceive();
    lyraDomainRun(origin);
    lyraGcCollect();
    auto* sender = start(other, lyraChannelSend(secondChannel, &last));
    lyraDomainRun(other);
    check(lyraTaskState(cancelled) == LYRA_TASK_CANCELLED && lyraTaskState(sender) == LYRA_TASK_COMPLETED &&
        lyraTaskState(committed) == LYRA_TASK_READY, "a cancelled select stole the rendezvous");
    lyraTaskRequestCancel(committed);
    lyraDomainRun(origin);
    auto* choice = static_cast<LyraSelection*>(lyraTaskOutcome(committed));
    check(choice->arm == 1 && choice->outcome == &last, "a committed receive lost its message before its domain resumed");
    sender = start(other, lyraChannelSend(firstChannel, &initial));
    lyraDomainRun(other);
    check(lyraTaskState(sender) == LYRA_TASK_SUSPENDED, "losing select arm stayed in the receive queue");
    first = start(origin, receive(firstChannel));
    lyraDomainRun(origin);
    lyraDomainRun(other);
    check(lyraTaskOutcome(first) == &initial && lyraTaskState(sender) == LYRA_TASK_COMPLETED, "rendezvous duplicated or discarded a message");

    auto* conflicting = lyraTaskSelect(2);
    lyraTaskSelectArm(conflicting, 0, receive(firstChannel));
    lyraTaskSelectArm(conflicting, 1, lyraChannelSend(firstChannel, &removed));
    start(origin, conflicting);
    lyraDomainRun(origin);
    check(lyraTaskState(conflicting) == LYRA_TASK_SUSPENDED, "a select exchanged a message with its own losing arm");
    lyraChannelClose(firstChannel);
    lyraDomainRun(origin);
    choice = static_cast<LyraSelection*>(lyraTaskOutcome(conflicting));
    check(choice->arm == 0 && !choice->outcome, "close ignored source order when both arms became ready");
}

void checkSingleWorker() {
    check(uv_os_setenv("LYRA_PARALLEL_WORKERS", "1") == 0, "cannot set parallel Worker limit");
    auto caller = std::this_thread::get_id();
    auto *root = lyraTaskCreate([](LyraTask *self, void *storage) {
        auto *worker = lyraTaskCreate([](LyraTask *self, void *storage) {
            auto& caller = *static_cast<std::thread::id *>(storage);
            check(std::this_thread::get_id() != caller, "configured Worker ran on the root executor");
            auto *frame = static_cast<NestedDomain *>(lyraGcAlloc(sizeof(NestedDomain)));
            *frame = {8, nullptr};
            lyraTaskParallel(self, lyraTaskCreate(nestedDomain, frame));
            lyraTaskComplete(self, nullptr);
        }, storage);
        lyraTaskParallel(self, worker);
        lyraTaskComplete(self, nullptr);
    }, &caller);
    check(lyraTaskRunRoot(root) == 0, "nested parallel work deadlocked with one Worker");
}

}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--single-worker") == 0) {
        check(lyraTaskParallelism() == 1, "parallelism API ignored the one-Worker configuration");
        checkSingleWorker();
        return EXIT_SUCCESS;
    }
    check(uv_os_setenv("LYRA_PARALLEL_WORKERS", "2") == 0, "cannot configure parallel tests");
    check(lyraTaskParallelism() == 2, "parallelism API reported the wrong Worker capacity");
    auto *origin = lyraDomainCreate();
    auto *other = lyraDomainCreate();
    unsigned executions = 0;
    unsigned cleanups = 0;
    auto *target = producer(&executions, false);
    auto *local = observer(target, &cleanups);
    auto *remote = observer(target, &cleanups);
    auto *abandonedWait = observer(target, &cleanups);
    check(executions == 0 && lyraTaskState(target) == LYRA_TASK_COLD, "creating a task executed its body");

    lyraTaskStart(origin, local);
    lyraDomainRun(origin);
    check(executions == 1 && lyraTaskState(target) == LYRA_TASK_SUSPENDED && lyraTaskState(local) == LYRA_TASK_SUSPENDED, "await did not activate and suspend the cold target");
    lyraTaskStart(other, target);
    lyraTaskStart(other, remote);
    lyraTaskStart(other, abandonedWait);
    lyraDomainRun(other);
    lyraTaskRequestCancel(abandonedWait);
    lyraTaskRequestCancel(abandonedWait);
    lyraDomainRun(other);
    check(cleanups == 1 && lyraTaskState(abandonedWait) == LYRA_TASK_CANCELLED, "cancelled waiter did not clean up exactly once");
    check(!lyraTaskCancelRequested(target), "cancelling a waiter cancelled another domain's task");

    collectUnderPressure();
    lyraTaskWake(target);
    lyraTaskWake(target);
    lyraDomainRun(origin);
    check(lyraTaskState(local) == LYRA_TASK_COMPLETED && lyraTaskState(remote) == LYRA_TASK_READY, "cross-domain waiter did not resume on its own domain");
    lyraDomainRun(other);
    check(lyraTaskOutcome(remote) == lyraTaskOutcome(local), "waiters received different object identities");
    check(static_cast<Counter *>(lyraTaskOutcome(remote))->count == 173, "captured object was lost while suspended");

    auto *late = observer(target, &cleanups);
    lyraTaskStart(other, late);
    lyraDomainRun(other);
    check(executions == 1 && lyraTaskOutcome(late) == lyraTaskOutcome(local), "late await recomputed the value");

    auto *failure = producer(&executions, true);
    auto *failedWaiter = observer(failure, &cleanups);
    lyraTaskStart(origin, failedWaiter);
    lyraDomainRun(origin);
    lyraTaskWake(failure);
    lyraDomainRun(origin);
    check(lyraTaskState(failedWaiter) == LYRA_TASK_FAILED && lyraTaskException(failedWaiter) == lyraTaskException(failure), "await lost the failure identity");

    unsigned counter = 0;
    auto *sibling = lyraTaskCreate(increment, &counter);
    SpawnFrame serial{origin, sibling, &counter};
    lyraTaskStart(origin, lyraTaskCreate(spawnThenIncrement, &serial));
    lyraDomainRun(origin);
    check(counter == 2, "two serial tasks did not both update the counter");

    auto *selfCancel = lyraTaskCreate([](LyraTask *self, void *) {
        lyraTaskRequestCancel(self);
        lyraTaskComplete(self, nullptr);
    }, nullptr);
    lyraTaskStart(origin, selfCancel);
    lyraDomainRun(origin);
    check(lyraTaskState(selfCancel) == LYRA_TASK_CANCELLED, "normal return swallowed a pending cancellation");

    auto *ticket = static_cast<NativeTicket *>(calloc(1, sizeof(NativeTicket)));
    handOff(origin, ticket);
    lyraDomainRun(origin);
    collectUnderPressure();
    lyraTaskWake(ticket->task);
    lyraDomainRun(origin);
    check(ticket->observed == 794, "native-only ticket's suspended task lost its managed frame");
    free(ticket);

    NativeGate gate;
    BlockingFrame blocking{&gate, false};
    auto *call = lyraTaskCreate(callThenClose, &blocking);
    CancellationFrame cancellation{call, &gate};
    lyraTaskStart(origin, call);
    lyraTaskStart(origin, lyraTaskCreate(cancelDuringCall, &cancellation));
    lyraDomainRun(origin);
    check(gate.closed && lyraTaskState(call) == LYRA_TASK_CANCELLED, "native cleanup did not complete cancellation");
    DisconnectCase disconnected{};
    disconnected.locallyClosed = static_cast<bool *>(lyraGcAlloc(sizeof(bool)));
    *disconnected.locallyClosed = false;
    check(uv_socketpair(SOCK_STREAM, 0, disconnected.sockets, UV_NONBLOCK_PIPE, UV_NONBLOCK_PIPE) == 0, "cannot create test sockets");
    disconnected.monitor = lyraRuntime::net::disconnectMonitor(disconnected.sockets[0]);
    check(disconnected.monitor != 0, "cannot create disconnect monitor");
    disconnected.handler = lyraTaskCreate(watchedHandler, &disconnected);
    lyraTaskKeepCleanups(disconnected.handler, &disconnected.cleanups);
    lyraTaskStart(origin, disconnected.handler);
    lyraTaskStart(origin, lyraTaskCreate(observeDisconnect, &disconnected));
    lyraDomainRun(origin);
    check(disconnected.closed && lyraTaskState(disconnected.handler) == LYRA_TASK_CANCELLED, "request never finished cleanup");

    GroupCase group{};
    group.causes[0] = lyraExceptionCreate(lyraStringFromBytes("first", 5));
    group.causes[1] = lyraExceptionCreate(lyraStringFromBytes("second", 6));
    auto *parent = lyraTaskCreate(failGroup, &group);
    lyraTaskKeepCleanups(parent, &group.cleanups);
    lyraTaskStart(origin, parent);
    lyraDomainRun(origin);
    check(group.cleaned == 1, "group cleanup was lost");
    checkFailures(parent, group.causes[0], group.causes[1]);
    checkChannels(origin, other);
    checkParallel(origin);
    lyraDomainDestroy(other);
    lyraDomainDestroy(origin);
}
