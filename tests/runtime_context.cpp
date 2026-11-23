#include "context.h"
#include "managed.h"
#include "strings.h"
#include "tasks.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

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

int *number(int value) { return ::new (lyraGcAlloc(sizeof(int))) int(value); }

[[gnu::noinline]] LyraTask *withLocal(const void *identity, int value, LyraTask *body) {
    return lyraTaskWithLocal(identity, number(value), body);
}

int *localNumber(const void *identity, int expected) {
    auto *value = static_cast<int *>(lyraContextLocal(lyraContextCurrent(), identity));
    check(value && *value == expected, "task local was lost or visible in the wrong scope");
    return value;
}

struct LocalCase {
    const void *identity;
    LyraTask *scope{}, *failing{};
    const LyraException *failure{};
    unsigned steps{};
};

void scopedNumber(LyraTask *self, void *storage) {
    auto& test = *static_cast<LocalCase *>(storage);
    traceIs("activation");
    localNumber(&key, 5);
    auto *value = localNumber(test.identity, 73);
    if (++test.steps == 1) {
        lyraTaskSuspend(self);
        lyraTaskWake(self);
    } else {
        lyraGcCollect();
        check(value == localNumber(test.identity, 73), "resumed task lost its local value identity");
        lyraTaskComplete(self, value);
    }
}

void localRoot(LyraTask *self, void *storage) {
    auto& test = *static_cast<LocalCase *>(storage);
    if (!test.scope) {
        auto *outer = lyraContextWithLocal(lyraContextCurrent(), test.identity, number(23));
        check(lyraContextEnter(lyraContextWithTrace(outer, text("creation"))), "cannot install creation context");
        test.scope = withLocal(test.identity, 73, lyraTaskCreate(scopedNumber, &test));
        check(lyraTaskState(test.scope) == LYRA_TASK_COLD && !test.steps, "local scope started before activation");
        auto *activation = lyraContextWithLocal(lyraContextCurrent(), &key, number(5));
        check(lyraContextEnter(lyraContextWithTrace(activation, text("activation"))), "cannot install activation context");
        test.failure = lyraExceptionCreate(text("scoped failure"));
        auto *failing = lyraTaskCreate([](LyraTask *task, void *storage) {
            auto& test = *static_cast<LocalCase *>(storage);
            localNumber(test.identity, 99);
            lyraTaskFail(task, test.failure);
        }, &test);
        test.failing = withLocal(test.identity, 99, failing);
        lyraGcCollect();
    }
    if (lyraTaskAwaitValue(self, test.scope) == LYRA_AWAIT_PENDING) return;
    check(lyraTaskState(test.scope) == LYRA_TASK_COMPLETED && *static_cast<int *>(lyraTaskOutcome(test.scope)) == 73 && test.steps == 2,
          "scoped task did not forward its original outcome");
    localNumber(test.identity, 23);
    auto failure = lyraTaskAwaitValue(self, test.failing);
    if (failure == LYRA_AWAIT_PENDING) return;
    check(failure == LYRA_AWAIT_EXCEPTION && lyraTaskException(test.failing) == test.failure, "scoped task changed the exception");
    lyraTaskComplete(self, nullptr);
}

void checkExistingBody(LyraDomain *domain) {
    auto *origin = lyraDomainCreate();
    bool parked{};
    auto *body = lyraTaskCreate([](LyraTask *task, void *storage) {
        auto& parked = *static_cast<bool *>(storage);
        check(!lyraContextLocal(lyraContextCurrent(), &key), "already active body received an unrelated local");
        if (parked) lyraTaskComplete(task, nullptr);
        else { parked = true; lyraTaskSuspend(task); }
    }, &parked);
    lyraTaskStart(origin, body);
    lyraDomainRun(origin);
    auto *scope = withLocal(&key, 73, body);
    auto *unrelated = withLocal(&key, 23, body);
    lyraTaskStart(domain, scope);
    lyraTaskStart(domain, unrelated);
    lyraDomainRun(domain);
    lyraTaskRequestCancel(scope);
    lyraDomainRun(domain);
    check(lyraTaskState(scope) == LYRA_TASK_CANCELLED && !lyraTaskCancelRequested(body), "scope cancelled another domain's task");
    lyraTaskWake(body);
    lyraDomainRun(origin);
    lyraDomainRun(domain);
    check(lyraTaskState(unrelated) == LYRA_TASK_COMPLETED && lyraTaskState(body) == LYRA_TASK_COMPLETED,
          "unrelated scopes changed the existing task's result");
    lyraDomainDestroy(origin);
}

}

int main() {
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
    LocalCase local{lyraGcAlloc(1)};
    auto *locals = lyraTaskCreate(localRoot, &local);
    lyraTaskStart(domain, locals);
    lyraDomainRun(domain);
    check(lyraTaskState(locals) == LYRA_TASK_COMPLETED, "scoped locals did not complete");
    checkExistingBody(domain);
    lyraDomainDestroy(domain);
}
