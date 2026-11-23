#include "context.h"
#include "managed.h"
#include "strings.h"
#include "tasks.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

const char key = 0;
const void *text(const char *value) { return lyraStringFromBytes(value, std::strlen(value)); }
void check(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "context: %s\n", message); std::exit(EXIT_FAILURE); }
}
void traceIs(const char *expected) {
    check(lyraStringEqual(lyraContextTrace(lyraContextCurrent()), text(expected)), "wrong task trace");
}

struct ScopeCase {
    const LyraContext *expected;
    LyraTask *cold;
    bool childStarted{}, workerReturned{};
};

void worker(void *storage) {
    auto *work = static_cast<ScopeCase *>(storage);
    traceIs("B");
    lyraGcCollect();
    check(*static_cast<int *>(lyraContextLocal(lyraContextCurrent(), &key)) == 73, "lost managed local in worker");
    check(!lyraContextEnter(work->expected), "blocking worker mutated the domain");
    work->workerReturned = true;
}

void child(LyraTask *self, void *storage) {
    auto *work = static_cast<ScopeCase *>(storage);
    check(lyraContextCurrent() == work->expected, "cold task did not inherit activator's context");
    if (!work->childStarted) {
        work->childStarted = true;
        lyraTaskBlock(self, worker, storage);
    } else {
        check(work->workerReturned, "worker did not finish");
        traceIs("B");
        lyraTaskComplete(self, nullptr);
    }
}

void scoped(LyraTask *self, void *storage) {
    auto *work = static_cast<ScopeCase *>(storage);
    if (!work->expected) {
        traceIs("A");
        auto *base = lyraContextWithTrace(lyraContextCurrent(), text("B"));
        auto *value = static_cast<int *>(lyraGcAlloc(sizeof(int)));
        *value = 73;
        auto *local = lyraContextWithLocal(base, &key, value);
        auto *deadline = lyraContextWithTimeout(local, 60000);
        work->expected = lyraContextWithTimeout(deadline, 120000);
        check(lyraContextDeadline(work->expected) == lyraContextDeadline(deadline), "inner extended deadline");
        check(lyraContextEnter(work->expected) && !lyraContextEnter(local), "failed to narrow deadline");
    }
    if (lyraTaskAwait(self, work->cold)) lyraTaskComplete(self, nullptr);
}

void root(LyraTask *self, void *storage) {
    auto *work = static_cast<ScopeCase *>(storage);
    traceIs("");
    check(lyraContextEnter(lyraContextWithTrace(nullptr, text("A"))), "outside async scope");
    work->cold = lyraTaskCreate(child, storage);
    lyraTaskSpawn(self, lyraTaskCreate(scoped, storage));
    traceIs("A");
    check(!lyraContextLocal(lyraContextCurrent(), &key), "child local escaped scope");
    lyraTaskComplete(self, nullptr);
}

struct ExpiryCase { bool waiting{}; };
void expires(LyraTask *self, void *storage) {
    auto *work = static_cast<ExpiryCase *>(storage);
    if (work->waiting) {
        check(lyraTaskCancelRequested(self) && lyraContextRemaining(lyraContextCurrent()) == 0, "timer did not cancel suspended scope");
        lyraTaskFinishCancelled(self);
    } else {
        work->waiting = true;
        check(lyraContextEnter(lyraContextWithTimeout(nullptr, 2)), "timer installation failed");
        lyraTaskSuspend(self);
    }
}

}

int main() {
    alarm(10);
    check(!lyraContextCurrent() && !lyraContextEnter(nullptr), "sync caller has an active task");
    auto *domain = lyraDomainCreate();
    ScopeCase scopes{};
    auto *entry = lyraTaskCreate(root, &scopes);
    lyraTaskStart(domain, entry);
    lyraDomainRun(domain);
    check(lyraTaskState(entry) == LYRA_TASK_COMPLETED && scopes.workerReturned, "scoped worker run failed");
    check(!lyraContextCurrent(), "event loop leaked task context");
    ExpiryCase timer{};
    auto *bound = lyraTaskCreate(expires, &timer);
    lyraTaskStart(domain, bound);
    lyraDomainRun(domain);
    check(lyraTaskState(bound) == LYRA_TASK_CANCELLED, "deadline did not terminate task");
    lyraDomainDestroy(domain);
}
