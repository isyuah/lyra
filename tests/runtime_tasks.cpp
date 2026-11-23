#include "managed.h"
#include "strings.h"
#include "tasks.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "task test: %s\n", message);
        exit(EXIT_FAILURE);
    }
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
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
    bool entered = false;
    bool mayReturn = false;
    bool returned = false;
    bool closed = false;
};

void nativeCall(void *storage) {
    auto *gate = static_cast<NativeGate *>(storage);
    pthread_mutex_lock(&gate->lock);
    gate->entered = true;
    pthread_cond_signal(&gate->changed);
    while (!gate->mayReturn) pthread_cond_wait(&gate->changed, &gate->lock);
    check(!gate->closed, "cancel freed a handle still in use by a worker");
    gate->returned = true;
    pthread_mutex_unlock(&gate->lock);
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
    pthread_mutex_lock(&gate->lock);
    while (!gate->entered) pthread_cond_wait(&gate->changed, &gate->lock);
    lyraTaskRequestCancel(frame->target);
    lyraTaskWake(frame->target);
    check(lyraTaskState(frame->target) == LYRA_TASK_SUSPENDED && !gate->closed, "native request woke before completion");
    lyraGcCollect();
    gate->mayReturn = true;
    pthread_cond_signal(&gate->changed);
    pthread_mutex_unlock(&gate->lock);
    lyraTaskComplete(self, nullptr);
}

struct DisconnectCase {
    int sockets[2];
    NativeGate gate;
    LyraTask *handler{}, *blocked{};
    LyraCleanup *cleanups{};
    bool observed{}, closed{};
};

void *queuedClosure(void *storage, const LyraException **) {
    auto *test = static_cast<DisconnectCase *>(storage);
    check(!lyraTaskWatchDisconnect(test->sockets[0]), "worker shared the caller's current task");
    close(test->sockets[1]);
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
    pthread_mutex_lock(&test->gate.lock);
    check(!test->closed && lyraTaskCancelRequested(test->blocked), "disconnect did not cancel the worker's owner tree");
    check(lyraTaskState(test->handler) == LYRA_TASK_SUSPENDED && lyraTaskState(test->blocked) == LYRA_TASK_SUSPENDED,
          "pending C operation or its parent resumed before the worker returned");
    test->observed = true;
    test->gate.mayReturn = true;
    pthread_cond_signal(&test->gate.changed);
    pthread_mutex_unlock(&test->gate.lock);
    lyraTaskComplete(self, nullptr);
}

void watchedHandler(LyraTask *self, void *storage) {
    auto *test = static_cast<DisconnectCase *>(storage);
    if (!test->blocked) {
        lyraCleanupPush(&test->cleanups, [](void *environment) -> const LyraException * {
            auto *closing = static_cast<DisconnectCase *>(environment);
            check(closing->gate.returned && closing->observed && !closing->closed, "defer closed a resource before opaque native work ended");
            close(closing->sockets[0]);
            closing->closed = true;
            return nullptr;
        }, test);
        check(lyraTaskWatchDisconnect(test->sockets[0]), "owner could not watch a socket");
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

}

int main() {
    alarm(20);
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
    pthread_cond_destroy(&gate.changed);
    pthread_mutex_destroy(&gate.lock);

    DisconnectCase disconnected{};
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, disconnected.sockets) == 0, "cannot create test sockets");
    disconnected.handler = lyraTaskCreate(watchedHandler, &disconnected);
    lyraTaskKeepCleanups(disconnected.handler, &disconnected.cleanups);
    lyraTaskStart(origin, disconnected.handler);
    lyraTaskStart(origin, lyraTaskCreate(observeDisconnect, &disconnected));
    lyraDomainRun(origin);
    check(disconnected.closed && lyraTaskState(disconnected.handler) == LYRA_TASK_CANCELLED, "request never finished cleanup");
    pthread_cond_destroy(&disconnected.gate.changed);
    pthread_mutex_destroy(&disconnected.gate.lock);

    GroupCase group{};
    group.causes[0] = lyraExceptionCreate(lyraStringFromBytes("first", 5));
    group.causes[1] = lyraExceptionCreate(lyraStringFromBytes("second", 6));
    auto *parent = lyraTaskCreate(failGroup, &group);
    lyraTaskKeepCleanups(parent, &group.cleanups);
    lyraTaskStart(origin, parent);
    lyraDomainRun(origin);
    check(lyraTaskState(parent) == LYRA_TASK_FAILED && lyraTaskFailureCount(parent) == 2 && group.cleaned == 1, "unobserved failure or group cleanup was lost");
    check(lyraTaskFailureAt(parent, 0) == group.causes[0] && lyraTaskFailureAt(parent, 1) == group.causes[1],
          "failures were reported by completion instead of registration");
    lyraDomainDestroy(other);
    lyraDomainDestroy(origin);
}
