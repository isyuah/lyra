#include "tasks.h"
#include "context.h"
#include "locks.h"
#include "managed.h"

#include <array>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

struct Executor;
struct LyraDomain {
    Executor *executor;
    size_t live;
    bool pooled;
};

struct ParallelPool;
struct Executor {
    uv_loop_t loop;
    uv_idle_t idle;
    uv_async_t inbox;
    uv_thread_t thread;
    LyraTask *first;
    LyraTask *last;
    ParallelPool *pool;
    size_t leases, steps;
    bool worker, running, stopping;
};

struct ParallelPool { std::array<Executor, 2> workers; size_t next; };

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
#ifdef _WIN32
    uv_async_t ready;
    HANDLE event, waiter;
#else
    uv_poll_t poll;
    int fd;
#endif
    LyraTask *task;
    DisconnectWatch *next;
};

struct DeadlineWatch { uv_timer_t timer; LyraTask *task; };

namespace {

thread_local LyraTask *currentTask;
thread_local const LyraContext *workerContext;
thread_local Executor *currentExecutor;
uv_mutex_t taskState;
uv_once_t taskStateInitialized = UV_ONCE_INIT;
uv_mutex_t sharedAccess;
uv_once_t sharedInitialized = UV_ONCE_INIT;

void require(bool condition, const char *error) {
    if (!condition) {
        fprintf(stderr, "Lyra task runtime: %s\n", error);
        abort();
    }
}

class TaskLock {
public:
    TaskLock() {
        uv_once(&taskStateInitialized, [] { lyraRuntime::initMutex(taskState); });
        lock();
    }
    ~TaskLock() { unlock(); }
    TaskLock(const TaskLock&) = delete;
    TaskLock& operator=(const TaskLock&) = delete;
    void lock() { uv_mutex_lock(&taskState); }
    void unlock() { uv_mutex_unlock(&taskState); }
};

template<class Storage>
Storage *allocateRuntime(const char *error) {
    auto *storage = calloc(1, sizeof(Storage));
    require(storage != nullptr, error);
    return static_cast<Storage *>(storage);
}

bool isTerminal(const LyraTask *task) {
    return task->state == LYRA_TASK_COMPLETED || task->state == LYRA_TASK_FAILED || task->state == LYRA_TASK_CANCELLED;
}

void dispatch(uv_idle_t *idle);
void publish(TaskLock& lock, LyraTask *task);
void requestCancel(LyraTask *task);

void refresh(Executor& executor) {
    if (executor.first) require(uv_idle_start(&executor.idle, dispatch) == 0, "cannot schedule a domain step");
    else uv_idle_stop(&executor.idle);
    auto *inbox = reinterpret_cast<uv_handle_t *>(&executor.inbox);
    if (executor.worker || executor.leases) uv_ref(inbox);
    else uv_unref(inbox);
}

void notify(Executor& executor) {
    if (currentExecutor == &executor) refresh(executor);
    else require(uv_async_send(&executor.inbox) == 0, "cannot notify execution domain");
}

void closeExecutor(Executor& executor) {
    require(!executor.first && !executor.leases, "cannot close executor with pending work");
    uv_close(reinterpret_cast<uv_handle_t *>(&executor.idle), nullptr);
    uv_close(reinterpret_cast<uv_handle_t *>(&executor.inbox), nullptr);
}

void initExecutor(Executor& executor, bool worker) {
    executor.worker = worker;
    require(uv_loop_init(&executor.loop) == 0, "cannot initialize event loop");
    require(uv_idle_init(&executor.loop, &executor.idle) == 0, "cannot initialize ready queue");
    require(uv_async_init(&executor.loop, &executor.inbox, [](uv_async_t *inbox) {
        TaskLock lock;
        auto& executor = *static_cast<Executor *>(inbox->data);
        if (executor.stopping) closeExecutor(executor);
        else refresh(executor);
    }) == 0, "cannot initialize domain inbox");
    executor.idle.data = executor.inbox.data = &executor;
    if (!worker) uv_unref(reinterpret_cast<uv_handle_t *>(&executor.inbox));
}

void workExecutor(void *storage) {
    auto& executor = *static_cast<Executor *>(storage);
    auto registration = lyraGcEnterWorker();
    currentExecutor = &executor;
    uv_run(&executor.loop, UV_RUN_DEFAULT);
    require(uv_loop_close(&executor.loop) == 0, "worker event loop has unclosed handles");
    currentExecutor = nullptr;
    lyraGcLeaveWorker(registration);
}

ParallelPool& parallelPool(Executor& executor) {
    if (executor.pool) return *executor.pool;
    auto *pool = allocateRuntime<ParallelPool>("cannot allocate parallel executor pool");
    executor.pool = pool;
    for (auto& worker : pool->workers) {
        worker.pool = pool;
        initExecutor(worker, true);
        require(uv_thread_create(&worker.thread, workExecutor, &worker) == 0, "cannot start parallel executor");
    }
    return *pool;
}

void transferLease(LyraDomain *receiver, LyraDomain *sender, bool acquire) {
    auto& executor = *receiver->executor;
    if (executor.worker || !sender->executor->worker) return;
    if (acquire) ++executor.leases;
    else --executor.leases;
    notify(executor);
}

void dispatch(uv_idle_t *idle) {
    TaskLock lock;
    auto& executor = *static_cast<Executor *>(idle->data);
    auto *task = executor.first;
    executor.first = task->nextReady;
    task->nextReady = nullptr;
    if (!executor.first) {
        executor.last = nullptr;
        uv_idle_stop(&executor.idle);
    }
    task->state = LYRA_TASK_RUNNING;
    if (task->joining) { publish(lock, task); return; }
    if (!executor.worker) ++executor.steps;
    if (task->cancelRequested && task->firstChild) {
        task->state = LYRA_TASK_SUSPENDED;
        task->resumeAfterChildren = true;
        return;
    }
    auto *previous = currentTask;
    currentTask = task;
    lock.unlock();
    task->step(task, task->frame);
    currentTask = previous;
    lock.lock();
    require(task->state != LYRA_TASK_RUNNING, "step returned without completing or suspending");
}

void enqueue(LyraTask *task) {
    auto& executor = *task->domain->executor;
    task->state = LYRA_TASK_READY;
    if (executor.last) executor.last->nextReady = task;
    else executor.first = task;
    executor.last = task;
    notify(executor);
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
    TaskLock lock;
    task->blocking = false;
    enqueue(task);
}

void detachWaiter(LyraTask *waiter) {
    transferLease(waiter->domain, waiter->waitingOn->domain, false);
    auto **link = &waiter->waitingOn->firstWaiter;
    while (*link != waiter) link = &(*link)->nextWaiter;
    *link = waiter->nextWaiter;
    waiter->waitingOn = nullptr;
    waiter->nextWaiter = nullptr;
}

void startTask(LyraDomain *domain, LyraTask *task) {
    if (task->state != LYRA_TASK_COLD) return;
    task->activeRoot = lyraGcRootCreate(task);
    task->domain = domain;
    task->context = task->owner ? task->owner->context : nullptr;
    ++domain->live;
    enqueue(task);
}

void startChild(LyraTask *owner, LyraTask *child, bool reportToGroup, LyraDomain *domain) {
    if (child->state != LYRA_TASK_COLD) return;
    child->owner = owner;
    child->reportToGroup = reportToGroup;
    child->ordinal = owner->nextOrdinal++;
    child->previousChild = owner->lastChild;
    if (owner->lastChild) owner->lastChild->nextChild = child;
    else owner->firstChild = child;
    owner->lastChild = child;
    if (owner->cancelRequested || owner->failures) child->cancelRequested = true;
    transferLease(owner->domain, domain, true);
    startTask(domain, child);
}

void addFailure(Failure *&head, size_t ordinal, const LyraException *cause) {
    auto **slot = &head;
    while (*slot && (*slot)->ordinal <= ordinal) slot = &(*slot)->next;
    *slot = ::new (lyraGcAlloc(sizeof(Failure))) Failure{ordinal, cause, *slot};
}

void closeWatch(DisconnectWatch *watch) {
#ifdef _WIN32
    if (watch->waiter) require(UnregisterWaitEx(watch->waiter, INVALID_HANDLE_VALUE) != 0, "cannot stop disconnect wait");
    CloseHandle(watch->event);
    uv_close(reinterpret_cast<uv_handle_t *>(&watch->ready), [](uv_handle_t *handle) { free(handle->data); });
#else
    uv_poll_stop(&watch->poll);
    uv_close(reinterpret_cast<uv_handle_t *>(&watch->poll), [](uv_handle_t *handle) {
        auto *closing = static_cast<DisconnectWatch *>(handle->data);
        close(closing->fd);
        free(closing);
    });
#endif
}

void stopDeadline(LyraTask *task) {
    if (auto *watch = task->deadlineWatch) {
        task->deadlineWatch = nullptr;
        uv_timer_stop(&watch->timer);
        uv_close(reinterpret_cast<uv_handle_t *>(&watch->timer), [](uv_handle_t *handle) { free(handle->data); });
    }
}

void armDeadline(LyraTask *task, int64_t remaining) {
    if (remaining == 0) { requestCancel(task); return; }
    auto *watch = static_cast<DeadlineWatch *>(calloc(1, sizeof(DeadlineWatch)));
    require(watch != nullptr, "cannot allocate deadline timer");
    auto& loop = task->domain->executor->loop;
    require(uv_timer_init(&loop, &watch->timer) == 0, "cannot initialize deadline timer");
    watch->task = task;
    watch->timer.data = watch;
    task->deadlineWatch = watch;
    uv_update_time(&loop);
    require(uv_timer_start(&watch->timer, [](uv_timer_t *timer) {
        lyraTaskRequestCancel(static_cast<DeadlineWatch *>(timer->data)->task);
    }, static_cast<uint64_t>(remaining), 0) == 0, "cannot schedule deadline");
}

void finish(TaskLock& lock, LyraTask *task, LyraTaskState state, void *outcome) {
    require(task->state == LYRA_TASK_RUNNING, "only a running task can finish");
    task->pendingState = state;
    task->outcome = outcome;
    if (task->firstChild) {
        task->joining = true;
        task->state = LYRA_TASK_SUSPENDED;
        if (state != LYRA_TASK_COMPLETED) for (auto *child = task->firstChild; child; child = child->nextChild) requestCancel(child);
        return;
    }
    publish(lock, task);
}

void publish(TaskLock& lock, LyraTask *task) {
    require(!task->firstChild, "task still has children to clean up");
    if (task->error) addFailure(task->failures, 0, task->error);
    const LyraException *primary = nullptr;
    for (auto *cause = task->failures; cause; cause = cause->next) primary = lyraExceptionSuppress(primary, cause->cause);
    for (auto *cause = task->cancelledCleanups; cause; cause = cause->next) primary = lyraExceptionSuppress(primary, cause->cause);
    if (auto **stack = task->cleanups) {
        task->cleanups = nullptr;
        auto *previous = currentTask;
        currentTask = task;
        lock.unlock();
        auto *cleaned = lyraCleanupUntil(stack, nullptr, primary);
        lock.lock();
        currentTask = previous;
        if (!primary && cleaned) addFailure(task->failures, 0, cleaned);
        primary = cleaned;
    }
    auto terminal = task->pendingState;
    if (terminal == LYRA_TASK_COMPLETED && task->cancelRequested) terminal = LYRA_TASK_CANCELLED;
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
    auto *domain = task->domain;
    if (auto *parent = task->owner) {
        transferLease(parent->domain, domain, false);
        if (task->previousChild) task->previousChild->nextChild = task->nextChild;
        else parent->firstChild = task->nextChild;
        if (task->nextChild) task->nextChild->previousChild = task->previousChild;
        else parent->lastChild = task->previousChild;
        task->owner = task->nextChild = task->previousChild = nullptr;
        if (task->state == LYRA_TASK_FAILED && task->reportToGroup) {
            addFailure(parent->failures, task->ordinal + 1, task->error);
            for (auto *sibling = parent->firstChild; sibling; sibling = sibling->nextChild) requestCancel(sibling);
        } else if (task->error && (parent->cancelRequested || parent->failures || (parent->joining && parent->pendingState == LYRA_TASK_FAILED))) {
            addFailure(parent->cancelledCleanups, task->ordinal + 1, task->error);
        }
        if (!parent->firstChild && (parent->joining || parent->resumeAfterChildren)) {
            parent->resumeAfterChildren = false;
            enqueue(parent);
        }
    }
    task->domain = nullptr;
    if (--domain->live == 0 && domain->pooled) free(domain);
    auto *root = task->activeRoot;
    task->activeRoot = nullptr;
    lyraGcRootDrop(root);
}

void requestCancel(LyraTask *task) {
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

int awaitTask(LyraTask *waiter, LyraTask *target) {
    require(waiter->state == LYRA_TASK_RUNNING && waiter != target, "invalid task wait");
    if (waiter->cancelRequested) { enqueue(waiter); return 0; }
    if (isTerminal(target)) return 1;
    startChild(waiter, target, false, waiter->domain);
    if (target->owner == waiter) target->reportToGroup = false;
    waiter->nextWaiter = target->firstWaiter;
    target->firstWaiter = waiter;
    waiter->waitingOn = target;
    waiter->state = LYRA_TASK_SUSPENDED;
    transferLease(waiter->domain, target->domain, true);
    return 0;
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
    if (lyraTaskCancelRequested(self)) {
        TaskLock lock;
        self->error = closure->error;
        finish(lock, self, LYRA_TASK_CANCELLED, nullptr);
    }
    else if (closure->error) lyraTaskFail(self, closure->error);
    else if (closure->submitted) lyraTaskComplete(self, closure->result);
    else { closure->submitted = true; lyraTaskBlock(self, runClosure, closure); }
}

struct LocalScope {
    const void *key;
    void *boxed;
    LyraTask *body;
    bool installed;
};

void localScopeStep(LyraTask *self, void *storage) {
    auto *scope = static_cast<LocalScope *>(storage);
    if (lyraTaskCancelRequested(self)) { lyraTaskFinishCancelled(self); return; }
    if (!scope->installed) {
        require(lyraContextEnter(lyraContextWithLocal(lyraContextCurrent(), scope->key, scope->boxed)), "cannot install task local scope");
        scope->installed = true;
    }
    switch (lyraTaskAwaitValue(self, scope->body)) {
        case LYRA_AWAIT_PENDING: return;
        case LYRA_AWAIT_VALUE: lyraTaskComplete(self, lyraTaskOutcome(scope->body)); return;
        case LYRA_AWAIT_EXCEPTION: lyraTaskFail(self, lyraTaskException(scope->body)); return;
    }
}

}

const LyraContext *lyraContextCurrent() { return currentTask ? currentTask->context : workerContext; }

uint8_t lyraContextEnter(const LyraContext *context) {
    if (!currentTask) return false;
    TaskLock lock;
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
    auto *executor = allocateRuntime<Executor>("cannot allocate executor");
    auto *domain = allocateRuntime<LyraDomain>("cannot allocate execution domain");
    domain->executor = executor;
    initExecutor(*executor, false);
    return domain;
}

size_t lyraDomainRun(LyraDomain *domain) {
    auto& executor = *domain->executor;
    {
        TaskLock lock;
        require(!domain->pooled && !executor.running && !currentExecutor, "event loop is not reentrant");
        executor.running = true;
        executor.steps = 0;
        refresh(executor);
    }
    currentExecutor = &executor;
    uv_run(&executor.loop, UV_RUN_DEFAULT);
    currentExecutor = nullptr;
    TaskLock lock;
    auto steps = executor.steps;
    executor.running = false;
    return steps;
}

void lyraDomainDestroy(LyraDomain *domain) {
    auto *executor = domain->executor;
    {
        TaskLock lock;
        require(!domain->pooled && domain->live == 0 && !executor->running, "cannot close a domain with live tasks or a running event loop");
        if (executor->pool) for (auto& worker : executor->pool->workers) {
            worker.stopping = true;
            require(uv_async_send(&worker.inbox) == 0, "cannot stop parallel executor");
        }
        closeExecutor(*executor);
    }
    if (auto *pool = executor->pool) {
        for (auto& worker : pool->workers) require(uv_thread_join(&worker.thread) == 0, "cannot join parallel executor");
        free(pool);
    }
    uv_run(&executor->loop, UV_RUN_DEFAULT);
    require(uv_loop_close(&executor->loop) == 0, "event loop has unclosed handles");
    free(executor);
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
    TaskLock lock;
    task->cleanups = stack;
}

void lyraTaskCreatedAt(LyraTask *task, const char *file, uint64_t line, uint64_t column) {
    TaskLock lock;
    task->createdFile = file;
    task->createdLine = line;
    task->createdColumn = column;
}

LyraTask *lyraTaskBlocking(LyraBlockingBody body, void *environment) {
    auto *closure = ::new (lyraGcAlloc(sizeof(BlockingClosure))) BlockingClosure{body, environment, nullptr, nullptr, false};
    return lyraTaskCreate(blockingStep, closure);
}

LyraTask *lyraTaskWithLocal(const void *key, void *boxed, LyraTask *body) {
    auto *scope = ::new (lyraGcAlloc(sizeof(LocalScope))) LocalScope{key, boxed, body, false};
    return lyraTaskCreate(localScopeStep, scope);
}

void lyraTaskSharedCopy(void *destination, const void *source, size_t bytes) {
    uv_once(&sharedInitialized, [] { lyraRuntime::initMutex(sharedAccess); });
    lyraRuntime::Lock lock(sharedAccess);
    memcpy(destination, source, bytes);
}

int lyraTaskWatchDisconnect(uintptr_t monitor) {
    if (!currentTask || !monitor) return 0;
    auto *watch = static_cast<DisconnectWatch *>(calloc(1, sizeof(DisconnectWatch)));
    require(watch != nullptr, "cannot allocate disconnect watch");
    watch->task = currentTask;
    auto *loop = &currentTask->domain->executor->loop;
#ifdef _WIN32
    auto *process = GetCurrentProcess();
    if (!DuplicateHandle(process, reinterpret_cast<HANDLE>(monitor), process, &watch->event, 0, FALSE, DUPLICATE_SAME_ACCESS)) { free(watch); return 0; }
    if (uv_async_init(loop, &watch->ready, [](uv_async_t *handle) {
        uv_unref(reinterpret_cast<uv_handle_t *>(handle));
        lyraTaskRequestCancel(static_cast<DisconnectWatch *>(handle->data)->task);
    })) { CloseHandle(watch->event); free(watch); return 0; }
    watch->ready.data = watch;
    if (!RegisterWaitForSingleObject(&watch->waiter, watch->event, [](void *data, BOOLEAN) {
        uv_async_send(&static_cast<DisconnectWatch *>(data)->ready);
    }, watch, INFINITE, WT_EXECUTEONLYONCE)) { closeWatch(watch); return 0; }
#else
    watch->fd = fcntl(static_cast<int>(monitor - 1), F_DUPFD_CLOEXEC, 0);
    if (watch->fd < 0) { free(watch); return 0; }
    if (uv_poll_init_socket(loop, &watch->poll, watch->fd)) { close(watch->fd); free(watch); return 0; }
    watch->poll.data = watch;
    int status = uv_poll_start(&watch->poll, UV_DISCONNECT, [](uv_poll_t *poll, int status, int events) {
        auto *watch = static_cast<DisconnectWatch *>(poll->data);
        if (status < 0 || (events & UV_DISCONNECT)) {
            uv_poll_stop(poll);
            lyraTaskRequestCancel(watch->task);
        }
    });
    if (status) { closeWatch(watch); return 0; }
#endif
    watch->next = currentTask->watches;
    currentTask->watches = watch;
    return 1;
}

void lyraTaskStart(LyraDomain *domain, LyraTask *task) {
    TaskLock lock;
    startTask(domain, task);
}

LyraTask *lyraTaskSpawn(LyraTask *parent, LyraTask *target) {
    TaskLock lock;
    require(parent->state == LYRA_TASK_RUNNING && currentTask == parent, "spawn requires the current running task");
    startChild(parent, target, true, parent->domain);
    return target;
}

LyraTask *lyraTaskParallel(LyraTask *parent, LyraTask *target) {
    TaskLock lock;
    require(parent->state == LYRA_TASK_RUNNING && currentTask == parent, "parallel requires the current running task");
    if (target->state != LYRA_TASK_COLD) return target;
    auto& host = *parent->domain->executor;
    auto& pool = parallelPool(host);
    auto next = [&]() -> Executor* { return &pool.workers[pool.next++ % pool.workers.size()]; };
    auto *executor = next();
    if (executor == &host) executor = next();
    auto *domain = allocateRuntime<LyraDomain>("cannot allocate parallel execution domain");
    *domain = {executor, 0, true};
    startChild(parent, target, true, domain);
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
    TaskLock lock;
    return awaitTask(waiter, target);
}

LyraAwaitStatus lyraTaskAwaitValue(LyraTask *waiter, LyraTask *target) {
    TaskLock lock;
    if (!awaitTask(waiter, target)) return LYRA_AWAIT_PENDING;
    if (target->state == LYRA_TASK_FAILED) return LYRA_AWAIT_EXCEPTION;
    if (target->state == LYRA_TASK_CANCELLED) {
        if (waiter->failures && !waiter->cancelRequested) {
            finish(lock, waiter, LYRA_TASK_FAILED, nullptr);
            return LYRA_AWAIT_PENDING;
        }
        waiter->error = target->error;
        requestCancel(waiter);
        finish(lock, waiter, LYRA_TASK_CANCELLED, nullptr);
        return LYRA_AWAIT_PENDING;
    }
    return LYRA_AWAIT_VALUE;
}

void lyraTaskSuspend(LyraTask *task) {
    TaskLock lock;
    require(task->state == LYRA_TASK_RUNNING, "only a running task can suspend");
    task->state = LYRA_TASK_SUSPENDED;
    if (task->cancelRequested) enqueue(task);
}

void lyraTaskWake(LyraTask *task) {
    TaskLock lock;
    if (task->state == LYRA_TASK_SUSPENDED && !task->waitingOn && !task->blocking && !task->joining && !task->resumeAfterChildren) enqueue(task);
}

void lyraTaskBlock(LyraTask *task, LyraNativeWork work, void *nativeData) {
    TaskLock lock;
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
    require(uv_queue_work(&task->domain->executor->loop, &request->request, runBlocking, finishBlocking) == 0, "cannot schedule native work");
}

void lyraTaskComplete(LyraTask *task, void *value) {
    TaskLock lock;
    if (task->cancelRequested) finish(lock, task, LYRA_TASK_CANCELLED, nullptr);
    else finish(lock, task, LYRA_TASK_COMPLETED, value);
}

void lyraTaskFail(LyraTask *task, const LyraException *error) {
    TaskLock lock;
    task->error = error;
    finish(lock, task, LYRA_TASK_FAILED, nullptr);
}

void lyraTaskRequestCancel(LyraTask *task) {
    TaskLock lock;
    requestCancel(task);
}

int lyraTaskCancelRequested(const LyraTask *task) {
    TaskLock lock;
    return task->cancelRequested;
}

void lyraTaskFinishCancelled(LyraTask *task) {
    TaskLock lock;
    require(task->cancelRequested, "task has no cancellation request");
    finish(lock, task, LYRA_TASK_CANCELLED, nullptr);
}

LyraTaskState lyraTaskState(const LyraTask *task) {
    TaskLock lock;
    return task->state;
}

void *lyraTaskOutcome(const LyraTask *task) {
    TaskLock lock;
    require(task->state == LYRA_TASK_COMPLETED, "task has no successful outcome");
    return task->outcome;
}

const LyraException *lyraTaskException(const LyraTask *task) {
    TaskLock lock;
    require(isTerminal(task), "task has not finished cleanup");
    return task->error;
}

size_t lyraTaskFailureCount(const LyraTask *task) {
    TaskLock lock;
    require(isTerminal(task), "task has not finished cleanup");
    size_t count = 0;
    for (auto *entry = task->failures; entry; entry = entry->next) ++count;
    return count;
}

const LyraException *lyraTaskFailureAt(const LyraTask *task, size_t index) {
    TaskLock lock;
    require(isTerminal(task), "task has not finished cleanup");
    auto *entry = task->failures;
    while (entry && index--) entry = entry->next;
    require(entry, "failure index is out of range");
    return entry->cause;
}
