#include "tasks.h"
#include "context.h"
#include "managed.h"

#include <fcntl.h>
#include <new>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uv.h>

struct LyraDomain {
    uv_loop_t loop;
    uv_idle_t idle;
    LyraTask *first;
    LyraTask *last;
    size_t live;
    size_t steps;
};

struct DisconnectWatch;
struct DeadlineWatch;
struct Failure {
    size_t ordinal;
    const LyraException *cause;
    Failure *next;
};

struct LyraTask {
    LyraTaskStep step;
    void *frame;
    void *outcome;
    const LyraException *error;
    const char *createdFile;
    uint64_t createdLine, createdColumn;
    bool reportToGroup;
    LyraDomain *domain;
    LyraGcRoot *activeRoot;
    LyraCleanup **cleanups;
    LyraTask *nextReady;
    LyraTask *firstWaiter;
    LyraTask *nextWaiter;
    LyraTask *waitingOn;
    LyraTask *owner, *firstChild, *lastChild, *previousChild, *nextChild;
    size_t ordinal, nextOrdinal;
    Failure *failures, *cancelledCleanups;
    DisconnectWatch *watches;
    const LyraContext *context;
    DeadlineWatch *deadlineWatch;
    LyraTaskState state;
    LyraTaskState pendingState;
    bool cancelRequested;
    bool blocking;
    bool joining, resumeAfterChildren;
};

struct DisconnectWatch {
    uv_poll_t poll;
    LyraTask *task;
    DisconnectWatch *next;
    int fd;
};

struct DeadlineWatch { uv_timer_t timer; LyraTask *task; };

namespace {

thread_local LyraTask *currentTask;
thread_local const LyraContext *workerContext;
pthread_mutex_t sharedAccess = PTHREAD_MUTEX_INITIALIZER;

void require(bool condition, const char *error) {
    if (!condition) {
        fprintf(stderr, "Lyra task runtime: %s\n", error);
        abort();
    }
}

bool isTerminal(const LyraTask *task) {
    return task->state == LYRA_TASK_COMPLETED || task->state == LYRA_TASK_FAILED || task->state == LYRA_TASK_CANCELLED;
}

void dispatch(uv_idle_t *idle) {
    auto *domain = static_cast<LyraDomain *>(idle->data);
    auto *task = domain->first;
    domain->first = task->nextReady;
    task->nextReady = nullptr;
    if (!domain->first) {
        domain->last = nullptr;
        uv_idle_stop(idle);
    }
    task->state = LYRA_TASK_RUNNING;
    ++domain->steps;
    if (task->cancelRequested && task->firstChild) {
        task->state = LYRA_TASK_SUSPENDED;
        task->resumeAfterChildren = true;
        return;
    }
    auto *previous = currentTask;
    currentTask = task;
    task->step(task, task->frame);
    currentTask = previous;
    require(task->state != LYRA_TASK_RUNNING, "step returned without completing or suspending");
}

void enqueue(LyraTask *task) {
    auto *domain = task->domain;
    task->state = LYRA_TASK_READY;
    if (domain->last) {
        domain->last->nextReady = task;
    } else {
        domain->first = task;
        require(uv_idle_start(&domain->idle, dispatch) == 0, "cannot schedule a domain step");
    }
    domain->last = task;
}

struct BlockingRequest {
    uv_work_t request;
    LyraTask *task;
    LyraNativeWork work;
    void *nativeData;
    LyraGcRoot *payloadRoot;
    const LyraContext *context;
};

void runBlocking(uv_work_t *work) {
    auto *request = static_cast<BlockingRequest *>(work->data);
    auto registration = lyraGcEnterWorker();
    auto *previous = workerContext;
    workerContext = request->context;
    request->work(request->nativeData);
    workerContext = previous;
    lyraGcLeaveWorker(registration);
}

void finishBlocking(uv_work_t *work, int status) {
    require(status == 0, "native work did not finish");
    auto *request = static_cast<BlockingRequest *>(work->data);
    auto *task = request->task;
    lyraGcRootDrop(request->payloadRoot);
    free(request);
    task->blocking = false;
    enqueue(task);
}

void detachWaiter(LyraTask *waiter) {
    auto **link = &waiter->waitingOn->firstWaiter;
    while (*link != waiter) link = &(*link)->nextWaiter;
    *link = waiter->nextWaiter;
    waiter->waitingOn = nullptr;
    waiter->nextWaiter = nullptr;
}

void startChild(LyraTask *owner, LyraTask *child, bool reportToGroup) {
    if (child->state != LYRA_TASK_COLD) return;
    child->owner = owner;
    child->reportToGroup = reportToGroup;
    child->ordinal = owner->nextOrdinal++;
    child->previousChild = owner->lastChild;
    if (owner->lastChild) owner->lastChild->nextChild = child;
    else owner->firstChild = child;
    owner->lastChild = child;
    if (owner->cancelRequested || owner->failures) child->cancelRequested = true;
    lyraTaskStart(owner->domain, child);
}

void addFailure(Failure *&head, size_t ordinal, const LyraException *cause) {
    auto **slot = &head;
    while (*slot && (*slot)->ordinal <= ordinal) slot = &(*slot)->next;
    *slot = ::new (lyraGcAlloc(sizeof(Failure))) Failure{ordinal, cause, *slot};
}

void closeWatch(DisconnectWatch *watch) {
    uv_poll_stop(&watch->poll);
    uv_close(reinterpret_cast<uv_handle_t *>(&watch->poll), [](uv_handle_t *handle) {
        auto *closing = static_cast<DisconnectWatch *>(handle->data);
        close(closing->fd);
        free(closing);
    });
}

void stopDeadline(LyraTask *task) {
    if (auto *watch = task->deadlineWatch) {
        task->deadlineWatch = nullptr;
        uv_timer_stop(&watch->timer);
        uv_close(reinterpret_cast<uv_handle_t *>(&watch->timer), [](uv_handle_t *handle) { free(handle->data); });
    }
}

void armDeadline(LyraTask *task, int64_t remaining) {
    if (remaining == 0) { lyraTaskRequestCancel(task); return; }
    auto *watch = static_cast<DeadlineWatch *>(calloc(1, sizeof(DeadlineWatch)));
    require(watch != nullptr, "cannot allocate deadline timer");
    require(uv_timer_init(&task->domain->loop, &watch->timer) == 0, "cannot initialize deadline timer");
    watch->task = task;
    watch->timer.data = watch;
    task->deadlineWatch = watch;
    uv_update_time(&task->domain->loop);
    require(uv_timer_start(&watch->timer, [](uv_timer_t *timer) {
        lyraTaskRequestCancel(static_cast<DeadlineWatch *>(timer->data)->task);
    }, static_cast<uint64_t>(remaining), 0) == 0, "cannot schedule deadline");
}

void finish(LyraTask *task, LyraTaskState state, void *outcome) {
    require(task->state == LYRA_TASK_RUNNING, "only a running task can finish");
    task->pendingState = state;
    task->outcome = outcome;
    if (task->firstChild) {
        task->joining = true;
        task->state = LYRA_TASK_SUSPENDED;
        if (state != LYRA_TASK_COMPLETED) for (auto *child = task->firstChild; child; child = child->nextChild) lyraTaskRequestCancel(child);
        return;
    }
    for (;;) {
        auto terminal = task->pendingState;
        if (task->error) addFailure(task->failures, 0, task->error);
        const LyraException *primary = nullptr;
        for (auto *cause = task->failures; cause; cause = cause->next) primary = lyraExceptionSuppress(primary, cause->cause);
        for (auto *cause = task->cancelledCleanups; cause; cause = cause->next) primary = lyraExceptionSuppress(primary, cause->cause);
        if (task->cleanups) {
            auto *previous = currentTask;
            currentTask = task;
            auto *cleaned = lyraCleanupUntil(task->cleanups, nullptr, primary);
            if (!primary && cleaned) addFailure(task->failures, 0, cleaned);
            primary = cleaned;
            currentTask = previous;
            task->cleanups = nullptr;
        }
        if (primary) {
            if (terminal != LYRA_TASK_CANCELLED) terminal = LYRA_TASK_FAILED;
            task->error = task->createdFile ? lyraExceptionFrame(primary, "created", task->createdFile, task->createdLine, task->createdColumn) : primary;
        }
        if (terminal != LYRA_TASK_COMPLETED) task->outcome = nullptr;
        task->state = terminal;
        task->frame = nullptr;
        task->step = nullptr;
        stopDeadline(task);
        task->context = nullptr;
        while (auto *watch = task->watches) {
            task->watches = watch->next;
            closeWatch(watch);
        }
        while (auto *waiter = task->firstWaiter) {
            detachWaiter(waiter);
            enqueue(waiter);
        }
        --task->domain->live;
        task->domain = nullptr;
        auto *parent = task->owner;
        if (parent) {
            if (task->previousChild) task->previousChild->nextChild = task->nextChild;
            else parent->firstChild = task->nextChild;
            if (task->nextChild) task->nextChild->previousChild = task->previousChild;
            else parent->lastChild = task->previousChild;
            task->owner = task->nextChild = task->previousChild = nullptr;
            if (task->state == LYRA_TASK_FAILED && task->reportToGroup) {
                addFailure(parent->failures, task->ordinal + 1, task->error);
                for (auto *sibling = parent->firstChild; sibling; sibling = sibling->nextChild) lyraTaskRequestCancel(sibling);
            } else if (task->error && (parent->cancelRequested || parent->failures || (parent->joining && parent->pendingState == LYRA_TASK_FAILED))) {
                addFailure(parent->cancelledCleanups, task->ordinal + 1, task->error);
            }
        }
        auto *root = task->activeRoot;
        task->activeRoot = nullptr;
        lyraGcRootDrop(root);
        if (!parent || parent->firstChild) return;
        if (parent->joining) { task = parent; continue; }
        if (parent->resumeAfterChildren) { parent->resumeAfterChildren = false; enqueue(parent); }
        return;
    }
}

struct BlockingClosure {
    LyraBlockingBody body;
    void *environment, *result;
    const LyraException *error;
    bool submitted;
};

void runClosure(void *storage) {
    auto *closure = static_cast<BlockingClosure *>(storage);
    closure->result = closure->body(closure->environment, &closure->error);
}

void blockingStep(LyraTask *self, void *storage) {
    auto *closure = static_cast<BlockingClosure *>(storage);
    if (lyraTaskCancelRequested(self)) { self->error = closure->error; lyraTaskFinishCancelled(self); }
    else if (closure->error) lyraTaskFail(self, closure->error);
    else if (closure->submitted) lyraTaskComplete(self, closure->result);
    else { closure->submitted = true; lyraTaskBlock(self, runClosure, closure); }
}

}

const LyraContext *lyraContextCurrent() { return currentTask ? currentTask->context : workerContext; }

uint8_t lyraContextEnter(const LyraContext *context) {
    if (!currentTask) return false;
    auto deadline = lyraContextDeadline(context);
    auto previous = lyraContextDeadline(currentTask->context);
    if (previous >= 0 && (deadline < 0 || previous < deadline)) return false;
    currentTask->context = context;
    if (deadline != previous) {
        stopDeadline(currentTask);
        armDeadline(currentTask, lyraContextRemaining(context));
    }
    return true;
}

LyraDomain *lyraDomainCreate() {
    auto *domain = static_cast<LyraDomain *>(calloc(1, sizeof(LyraDomain)));
    require(domain != nullptr, "cannot allocate execution domain");
    require(uv_loop_init(&domain->loop) == 0, "cannot initialize event loop");
    require(uv_idle_init(&domain->loop, &domain->idle) == 0, "cannot initialize ready queue");
    domain->idle.data = domain;
    return domain;
}

size_t lyraDomainRun(LyraDomain *domain) {
    require(domain->steps == 0, "event loop is not reentrant");
    uv_run(&domain->loop, UV_RUN_DEFAULT);
    auto steps = domain->steps;
    domain->steps = 0;
    return steps;
}

void lyraDomainDestroy(LyraDomain *domain) {
    require(domain->live == 0 && domain->steps == 0, "cannot close a domain with live tasks or a running event loop");
    uv_close(reinterpret_cast<uv_handle_t *>(&domain->idle), nullptr);
    uv_run(&domain->loop, UV_RUN_DEFAULT);
    require(uv_loop_close(&domain->loop) == 0, "event loop has unclosed handles");
    free(domain);
}

LyraTask *lyraTaskCreate(LyraTaskStep step, void *frame) {
    auto *task = ::new (lyraGcAlloc(sizeof(LyraTask))) LyraTask{};
    task->step = step;
    task->frame = frame;
    task->state = LYRA_TASK_COLD;
    return task;
}

void lyraTaskKeepCleanups(LyraTask *task, LyraCleanup **stack) {
    task->cleanups = stack;
}

void lyraTaskCreatedAt(LyraTask *task, const char *file, uint64_t line, uint64_t column) {
    task->createdFile = file;
    task->createdLine = line;
    task->createdColumn = column;
}

LyraTask *lyraTaskBlocking(LyraBlockingBody body, void *environment) {
    auto *closure = ::new (lyraGcAlloc(sizeof(BlockingClosure))) BlockingClosure{body, environment, nullptr, nullptr, false};
    return lyraTaskCreate(blockingStep, closure);
}

void lyraTaskSharedCopy(void *destination, const void *source, size_t bytes) {
    pthread_mutex_lock(&sharedAccess);
    memcpy(destination, source, bytes);
    pthread_mutex_unlock(&sharedAccess);
}

int lyraTaskWatchDisconnect(int socketFd) {
    if (!currentTask) return 0;
    auto fd = fcntl(socketFd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return 0;
    auto *watch = static_cast<DisconnectWatch *>(calloc(1, sizeof(DisconnectWatch)));
    require(watch != nullptr, "cannot allocate disconnect watch");
    watch->fd = fd;
    watch->task = currentTask;
    if (uv_poll_init(&currentTask->domain->loop, &watch->poll, fd)) { close(fd); free(watch); return 0; }
    watch->poll.data = watch;
    int status = uv_poll_start(&watch->poll, UV_DISCONNECT, [](uv_poll_t *poll, int status, int events) {
        auto *watch = static_cast<DisconnectWatch *>(poll->data);
        if (status < 0 || (events & UV_DISCONNECT)) {
            uv_poll_stop(poll);
            lyraTaskRequestCancel(watch->task);
        }
    });
    if (status) { closeWatch(watch); return 0; }
    watch->next = currentTask->watches;
    currentTask->watches = watch;
    return 1;
}

void lyraTaskStart(LyraDomain *domain, LyraTask *task) {
    if (task->state != LYRA_TASK_COLD) return;
    task->activeRoot = lyraGcRootCreate(task);
    task->domain = domain;
    task->context = task->owner ? task->owner->context : nullptr;
    ++domain->live;
    enqueue(task);
}

LyraTask *lyraTaskSpawn(LyraTask *parent, LyraTask *target) {
    startChild(parent, target, true);
    return target;
}

int lyraTaskRunRoot(LyraTask *task) {
    auto *domain = lyraDomainCreate();
    lyraTaskStart(domain, task);
    lyraDomainRun(domain);
    require(domain->live == 0, "no runnable work can complete the root task group");
    int status = task->state == LYRA_TASK_COMPLETED ? EXIT_SUCCESS : EXIT_FAILURE;
    if (task->state == LYRA_TASK_CANCELLED) fputs("Lyra runtime: async entry was cancelled\n", stderr);
    if (task->error) lyraExceptionReport(task->error);
    lyraDomainDestroy(domain);
    return status;
}

int lyraTaskAwait(LyraTask *waiter, LyraTask *target) {
    require(waiter->state == LYRA_TASK_RUNNING && waiter != target, "invalid task wait");
    if (waiter->cancelRequested) {
        enqueue(waiter);
        return 0;
    }
    if (isTerminal(target)) return 1;
    startChild(waiter, target, false);
    if (target->owner == waiter) target->reportToGroup = false;
    waiter->nextWaiter = target->firstWaiter;
    target->firstWaiter = waiter;
    waiter->waitingOn = target;
    waiter->state = LYRA_TASK_SUSPENDED;
    return 0;
}

LyraAwaitStatus lyraTaskAwaitValue(LyraTask *waiter, LyraTask *target) {
    if (!lyraTaskAwait(waiter, target)) return LYRA_AWAIT_PENDING;
    if (target->state == LYRA_TASK_FAILED) return LYRA_AWAIT_EXCEPTION;
    if (target->state == LYRA_TASK_CANCELLED) {
        if (waiter->failures && !waiter->cancelRequested) {
            finish(waiter, LYRA_TASK_FAILED, nullptr);
            return LYRA_AWAIT_PENDING;
        }
        waiter->error = target->error;
        lyraTaskRequestCancel(waiter);
        lyraTaskFinishCancelled(waiter);
        return LYRA_AWAIT_PENDING;
    }
    return LYRA_AWAIT_VALUE;
}

void lyraTaskSuspend(LyraTask *task) {
    require(task->state == LYRA_TASK_RUNNING, "only a running task can suspend");
    task->state = LYRA_TASK_SUSPENDED;
    if (task->cancelRequested) enqueue(task);
}

void lyraTaskWake(LyraTask *task) {
    if (task->state == LYRA_TASK_SUSPENDED && !task->waitingOn && !task->blocking && !task->joining && !task->resumeAfterChildren) enqueue(task);
}

void lyraTaskBlock(LyraTask *task, LyraNativeWork work, void *nativeData) {
    require(task->state == LYRA_TASK_RUNNING, "only a running task can schedule native work");
    if (task->cancelRequested) {
        enqueue(task);
        return;
    }
    auto *request = static_cast<BlockingRequest *>(calloc(1, sizeof(BlockingRequest)));
    require(request != nullptr, "cannot allocate native work request");
    request->task = task;
    request->work = work;
    request->nativeData = nativeData;
    request->payloadRoot = lyraGcRootCreate(nativeData);
    request->context = task->context;
    request->request.data = request;
    task->state = LYRA_TASK_SUSPENDED;
    task->blocking = true;
    require(uv_queue_work(&task->domain->loop, &request->request, runBlocking, finishBlocking) == 0, "cannot schedule native work");
}

void lyraTaskComplete(LyraTask *task, void *value) {
    if (task->cancelRequested) finish(task, LYRA_TASK_CANCELLED, nullptr);
    else finish(task, LYRA_TASK_COMPLETED, value);
}

void lyraTaskFail(LyraTask *task, const LyraException *error) {
    task->error = error;
    finish(task, LYRA_TASK_FAILED, nullptr);
}

void lyraTaskRequestCancel(LyraTask *task) {
    auto *current = task;
    for (;;) {
        if (!isTerminal(current) && !current->cancelRequested) {
            current->cancelRequested = true;
            if (current->joining && current->pendingState == LYRA_TASK_COMPLETED) current->pendingState = LYRA_TASK_CANCELLED;
            if (current->state == LYRA_TASK_SUSPENDED && !current->joining && !current->blocking) {
                if (current->waitingOn) detachWaiter(current);
                if (current->firstChild) current->resumeAfterChildren = true;
                else enqueue(current);
            }
            if (current->firstChild) { current = current->firstChild; continue; }
        }
        while (current != task && !current->nextChild) current = current->owner;
        if (current == task) return;
        current = current->nextChild;
    }
}

int lyraTaskCancelRequested(const LyraTask *task) {
    return task->cancelRequested;
}

void lyraTaskFinishCancelled(LyraTask *task) {
    require(task->cancelRequested, "task has no cancellation request");
    finish(task, LYRA_TASK_CANCELLED, nullptr);
}

LyraTaskState lyraTaskState(const LyraTask *task) {
    return task->state;
}

void *lyraTaskOutcome(const LyraTask *task) {
    require(task->state == LYRA_TASK_COMPLETED, "task has no successful outcome");
    return task->outcome;
}

const LyraException *lyraTaskException(const LyraTask *task) {
    require(isTerminal(task), "task has not finished cleanup");
    return task->error;
}

size_t lyraTaskFailureCount(const LyraTask *task) {
    require(isTerminal(task), "task has not finished cleanup");
    size_t count = 0;
    for (auto *entry = task->failures; entry; entry = entry->next) ++count;
    return count;
}

const LyraException *lyraTaskFailureAt(const LyraTask *task, size_t index) {
    require(isTerminal(task), "task has not finished cleanup");
    auto *entry = task->failures;
    while (entry && index--) entry = entry->next;
    require(entry, "failure index is out of range");
    return entry->cause;
}
