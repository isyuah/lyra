#include "managed.h"
#include "strings.h"
#include "tasks.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

void check(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "task exceptions: %s\n", message); std::exit(EXIT_FAILURE); }
}

const LyraException *raised(const char *message) {
    return lyraExceptionRaise(lyraExceptionCreate(lyraStringFromBytes(message, std::strlen(message))), "failure.ly", 2, 5);
}

struct Group {
    LyraTask *target{}, *sibling{};
    const LyraException *error = raised("child");
    bool spawnFirst{}, late{}, publish{}, caught{}, siblingCompleted{};
};

void failing(LyraTask *self, void *storage) {
    auto *group = static_cast<Group *>(storage);
    if (group->publish) lyraTaskFail(self, group->error);
    else lyraTaskSuspend(self);
}

void sibling(LyraTask *self, void *storage) {
    auto *group = static_cast<Group *>(storage);
    if (lyraTaskCancelRequested(self)) lyraTaskFinishCancelled(self);
    else if (group->caught) { group->siblingCompleted = true; lyraTaskComplete(self, nullptr); }
    else lyraTaskSuspend(self);
}

void observe(LyraTask *self, void *storage) {
    auto *group = static_cast<Group *>(storage);
    if (lyraTaskAwaitValue(self, group->target) == LYRA_AWAIT_PENDING) return;
    check(lyraTaskAwaitValue(self, group->target) == LYRA_AWAIT_EXCEPTION, "repeat await observed a successful completion");
    check(lyraTaskException(group->target) == group->error, "await changed the task's terminal exception");
    group->caught = true;
    lyraTaskWake(group->sibling);
    lyraTaskComplete(self, nullptr);
}

void owner(LyraTask *self, void *storage) {
    auto *group = static_cast<Group *>(storage);
    if (!group->target) {
        group->target = lyraTaskCreate(failing, group);
        if (group->spawnFirst) lyraTaskSpawn(self, group->target);
        group->sibling = lyraTaskSpawn(self, lyraTaskCreate(sibling, group));
        if (group->late) { lyraTaskSuspend(self); return; }
    }
    observe(self, storage);
}

void ownership(LyraDomain *origin, LyraDomain *other, bool spawnFirst, bool late) {
    Group group{};
    group.spawnFirst = spawnFirst;
    group.late = late;
    auto *parent = lyraTaskCreate(owner, &group);
    lyraTaskStart(origin, parent);
    lyraDomainRun(origin);
    auto *external = lyraTaskCreate(observe, &group);
    lyraTaskStart(other, external);
    lyraDomainRun(other);
    group.publish = true;
    lyraTaskWake(group.target);
    lyraDomainRun(origin);
    if (late) {
        check(lyraTaskState(group.sibling) == LYRA_TASK_CANCELLED, "cross-group await prevented fail-fast");
        lyraTaskWake(parent);
        lyraDomainRun(origin);
    }
    lyraDomainRun(other);
    check(group.caught && lyraTaskState(external) == LYRA_TASK_COMPLETED, "cross-domain failure could not be caught");
    if (late) {
        check(lyraTaskState(parent) == LYRA_TASK_FAILED && lyraTaskException(parent) == group.error, "late await erased group failure");
    } else {
        check(lyraTaskState(parent) == LYRA_TASK_COMPLETED && group.siblingCompleted, "observed failure was also reported to the group");
    }
}

struct Cleanup {
    const LyraException *failure;
    unsigned *count, order;
};

const LyraException *clean(void *storage) {
    auto *action = static_cast<Cleanup *>(storage);
    check(++*action->count == action->order, "cleanup order or count was wrong");
    return action->failure;
}

struct Exit {
    LyraTaskState intended;
    LyraCleanup *stack{};
    unsigned count{};
    const LyraException *primary = raised("body");
    Cleanup last{raised("first to run"), &count, 1}, first{raised("second to run"), &count, 2};
};

void exiting(LyraTask *self, void *storage) {
    auto *run = static_cast<Exit *>(storage);
    lyraCleanupPush(&run->stack, clean, &run->first);
    lyraCleanupPush(&run->stack, clean, &run->last);
    if (run->intended == LYRA_TASK_COMPLETED) lyraTaskComplete(self, nullptr);
    else if (run->intended == LYRA_TASK_FAILED) lyraTaskFail(self, run->primary);
    else { lyraTaskRequestCancel(self); lyraTaskFinishCancelled(self); }
}

void unwind(LyraDomain *domain, LyraTaskState intended) {
    Exit run{intended};
    auto *task = lyraTaskCreate(exiting, &run);
    lyraTaskKeepCleanups(task, &run.stack);
    lyraTaskCreatedAt(task, "caller.ly", 7, 11);
    lyraTaskStart(domain, task);
    lyraDomainRun(domain);
    auto *cause = lyraTaskException(task);
    check(run.count == 2 && !run.stack, "a failing cleanup skipped another cleanup");
    check(lyraTaskState(task) == (intended == LYRA_TASK_CANCELLED ? LYRA_TASK_CANCELLED : LYRA_TASK_FAILED), "cleanup swallowed cancellation or lost failure");
    check(lyraExceptionMessage(cause) == lyraExceptionMessage(intended == LYRA_TASK_FAILED ? run.primary : run.last.failure), "cleanup replaced the original cause");
    check(lyraExceptionSuppressedCount(cause) == (intended == LYRA_TASK_FAILED ? 2U : 1U), "a cleanup cause was not retained");
}

struct PeerFailure {
    LyraTask *victim{};
    LyraCleanup *stack{};
    const LyraException *failure = raised("unobserved sibling"), *cleanup = raised("cancelled cleanup");
};

void victim(LyraTask *self, void *storage) {
    auto *peers = static_cast<PeerFailure *>(storage);
    if (lyraTaskCancelRequested(self)) { lyraTaskFinishCancelled(self); return; }
    lyraCleanupPush(&peers->stack, [](void *capture) { return static_cast<PeerFailure *>(capture)->cleanup; }, peers);
    lyraTaskSuspend(self);
}

void failingPeer(LyraTask *self, void *storage) {
    auto *peers = static_cast<PeerFailure *>(storage);
    if (!peers->victim) {
        peers->victim = lyraTaskSpawn(self, lyraTaskCreate(victim, peers));
        lyraTaskKeepCleanups(peers->victim, &peers->stack);
        lyraTaskSpawn(self, lyraTaskCreate([](LyraTask *child, void *capture) { lyraTaskFail(child, static_cast<PeerFailure *>(capture)->failure); }, peers));
    }
    check(lyraTaskAwaitValue(self, peers->victim) == LYRA_AWAIT_PENDING, "ordinary catch swallowed a sibling's cancellation");
}

}

int main() {
    alarm(15);
    auto *origin = lyraDomainCreate(), *other = lyraDomainCreate();
    ownership(origin, other, false, false);
    ownership(origin, other, true, false);
    ownership(origin, other, true, true);
    unwind(origin, LYRA_TASK_COMPLETED);
    unwind(origin, LYRA_TASK_FAILED);
    unwind(origin, LYRA_TASK_CANCELLED);
    PeerFailure peers{};
    auto *group = lyraTaskCreate(failingPeer, &peers);
    lyraTaskStart(origin, group);
    lyraDomainRun(origin);
    check(lyraTaskState(group) == LYRA_TASK_FAILED && lyraTaskState(peers.victim) == LYRA_TASK_CANCELLED, "fail-fast cancelled the group instead of reporting its primary failure");
    auto *cause = lyraTaskException(group);
    check(lyraExceptionMessage(cause) == lyraExceptionMessage(peers.failure) && lyraExceptionSuppressedCount(cause) == 1, "sibling cleanup replaced the originating failure");
    auto *worker = lyraTaskBlocking([](void *, const LyraException **error) -> void * {
        *error = raised("worker");
        lyraGcCollect();
        return nullptr;
    }, nullptr);
    lyraTaskStart(origin, worker);
    lyraDomainRun(origin);
    check(lyraTaskState(worker) == LYRA_TASK_FAILED && lyraTaskException(worker), "worker exception was boxed as a normal result");
    lyraDomainDestroy(other);
    lyraDomainDestroy(origin);
}
