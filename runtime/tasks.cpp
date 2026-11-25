#include "tasks.h"
#include "context.h"
#include "locks.h"
#include "lists.h"
#include "managed.h"
#include "strings.h"

#include <algorithm>
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
struct ReadyDomains;
struct LyraDomain {
    Executor *executor;
    const void *cycleErrorType;
    LyraTask *first, *last;
    ReadyDomains *queued;
    LyraDomain *previousReady, *nextReady;
    size_t live, loopUsers;
    bool pooled, running;
};

struct ReadyDomains {
    LyraDomain *first, *last;

    void append(LyraDomain& domain) {
        domain.queued = this;
        domain.previousReady = last;
        if (last) last->nextReady = &domain;
        else first = &domain;
        last = &domain;
    }

    LyraDomain *remove(LyraDomain& domain) {
        if (domain.previousReady) domain.previousReady->nextReady = domain.nextReady;
        else first = domain.nextReady;
        if (domain.nextReady) domain.nextReady->previousReady = domain.previousReady;
        else last = domain.previousReady;
        domain.previousReady = domain.nextReady = nullptr;
        domain.queued = nullptr;
        return &domain;
    }
};

struct ParallelPool;
struct Executor {
    uv_loop_t loop;
    uv_idle_t idle;
    uv_async_t inbox;
    uv_thread_t thread;
    ReadyDomains pinned, movable;
    ParallelPool *pool;
    size_t leases, steps, nextVictim;
    bool worker, running, stepping, stopping, sleeping, preferMovable;
};

struct ParallelPool { Executor *workers; size_t workerCount, next; };

struct DisconnectWatch;
struct DeadlineWatch;
namespace { struct BlockingRequest; }
struct WaitSet;
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
    LyraTask *nextWaitSearch;
    WaitSet *wait;
    LyraTask *owner, *firstChild, *lastChild, *previousChild, *nextChild;
    size_t ordinal, nextOrdinal;
    Failure *failures, *cancelledCleanups;
    DisconnectWatch *watches;
    const LyraContext *context;
    DeadlineWatch *deadlineWatch;
    LyraTaskState state;
    LyraTaskState pendingState;
    bool cancelRequested;
    BlockingRequest *blocking;
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
    LyraDomain *domain;
    LyraGcRoot *target;
    int (*locallyClosed)(const void *);
    DisconnectWatch *next;
};

struct DeadlineWatch { uv_timer_t timer; LyraTask *task; LyraDomain *domain; };

enum class WaitKind { Send, Receive, Timer };
struct WaitChoice;
struct ChannelQueue { WaitChoice *first, *last; };

struct LyraChannel {
    const void *closedType;
    size_t capacity, allocated, head, count, nextTicket;
    void **items;
    ChannelQueue senders, receivers;
    bool closed;
};

struct WaitChoice {
    WaitKind kind;
    LyraChannel *channel;
    void *boxed;
    LyraChannelPack pack;
    int64_t time;
    const LyraTask *source;
    WaitSet *set;
    ChannelQueue *queue;
    WaitChoice *previous, *next;
    size_t ticket;
};

struct WaitTimer { uv_timer_t handle; LyraDomain *domain; WaitSet *set; };
struct WaitSet {
    LyraTask *task;
    WaitChoice *choices, *winner;
    size_t count;
    WaitTimer *timer;
    int64_t deadline;
    void *result;
    bool selecting, rejected;
};

namespace {

thread_local LyraTask *currentTask;
thread_local LyraTask *blockingTask;
thread_local const LyraContext *workerContext;
thread_local Executor *currentExecutor;
uv_mutex_t taskState;
uv_once_t taskStateInitialized = UV_ONCE_INIT;
uv_mutex_t sharedAccess;
uv_once_t sharedInitialized = UV_ONCE_INIT;
uv_once_t parallelWorkerCountInitialized = UV_ONCE_INIT;
size_t configuredParallelWorkerCount;

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
    if (executor.pinned.first || (executor.worker && !executor.sleeping))
        require(uv_idle_start(&executor.idle, dispatch) == 0, "cannot schedule a domain step");
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
    require(!executor.pinned.first && !executor.movable.first && !executor.leases, "cannot close executor with pending work");
    uv_close(reinterpret_cast<uv_handle_t *>(&executor.idle), nullptr);
    uv_close(reinterpret_cast<uv_handle_t *>(&executor.inbox), nullptr);
}

void initExecutor(Executor& executor, bool worker) {
    executor.worker = worker;
    executor.sleeping = worker;
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

void readParallelWorkerCount() {
    char text[16];
    size_t length = sizeof(text);
    auto status = uv_os_getenv("LYRA_PARALLEL_WORKERS", text, &length);
    if (status == UV_ENOENT) {
        configuredParallelWorkerCount = 2;
        return;
    }
    require(status == 0 && length > 0, "LYRA_PARALLEL_WORKERS must be an integer from 1 to 64");
    size_t configured = 0;
    for (size_t index = 0; index < length; ++index) {
        auto digit = text[index] - '0';
        require(digit >= 0 && digit <= 9 && configured <= 64,
                "LYRA_PARALLEL_WORKERS must be an integer from 1 to 64");
        configured = configured * 10 + static_cast<size_t>(digit);
    }
    require(configured >= 1 && configured <= 64, "LYRA_PARALLEL_WORKERS must be an integer from 1 to 64");
    configuredParallelWorkerCount = configured;
}

size_t parallelWorkerCount() {
    uv_once(&parallelWorkerCountInitialized, readParallelWorkerCount);
    return configuredParallelWorkerCount;
}

ParallelPool& parallelPool(Executor& executor) {
    if (executor.pool) return *executor.pool;
    auto *pool = allocateRuntime<ParallelPool>("cannot allocate parallel executor pool");
    pool->workerCount = parallelWorkerCount();
    pool->workers = static_cast<Executor *>(calloc(pool->workerCount, sizeof(Executor)));
    require(pool->workers != nullptr, "cannot allocate parallel executors");
    executor.pool = pool;
    for (size_t index = 0; index < pool->workerCount; ++index) {
        auto& worker = pool->workers[index];
        worker.pool = pool;
        initExecutor(worker, true);
        require(uv_thread_create(&worker.thread, workExecutor, &worker) == 0, "cannot start parallel executor");
    }
    return *pool;
}

void transferLease(LyraDomain *receiver, LyraDomain *sender, bool acquire) {
    if (receiver->pooled || !sender->pooled) return;
    auto& executor = *receiver->executor;
    if (acquire) ++executor.leases;
    else --executor.leases;
    notify(executor);
}

void wakeWorker(Executor& executor) {
    executor.sleeping = false;
    notify(executor);
}

void schedule(LyraDomain& domain) {
    if (domain.running || !domain.first) return;
    auto& executor = *domain.executor;
    bool stealable = domain.pooled && !domain.loopUsers;
    auto& queue = stealable ? executor.movable : executor.pinned;
    if (domain.queued == &queue) return;
    if (domain.queued) domain.queued->remove(domain);
    queue.append(domain);
    if (!executor.worker) notify(executor);
    else if (executor.sleeping) wakeWorker(executor);
    else if (stealable && (executor.stepping || executor.pinned.first || executor.movable.first != executor.movable.last)) for (size_t index = 0; index < executor.pool->workerCount; ++index) {
        auto& peer = executor.pool->workers[index];
        if (peer.sleeping && !peer.stopping) { wakeWorker(peer); break; }
    }
}

void settleDomain(LyraDomain& domain) {
    if (domain.running) return;
    if (domain.pooled && !domain.live && !domain.loopUsers) free(&domain);
    else schedule(domain);
}

void releaseWatchPin(LyraDomain& domain) {
    TaskLock lock;
    --domain.loopUsers;
    settleDomain(domain);
}

LyraDomain *nextDomain(Executor& executor) {
    if (auto *domain = executor.movable.first; domain && (executor.preferMovable || !executor.pinned.first)) {
        executor.preferMovable = false;
        return executor.movable.remove(*domain);
    }
    if (auto *domain = executor.pinned.first) {
        executor.preferMovable = true;
        return executor.pinned.remove(*domain);
    }
    if (!executor.worker) return nullptr;
    auto& pool = *executor.pool;
    for (size_t attempt = 0; attempt < pool.workerCount; ++attempt) {
        auto& victim = pool.workers[executor.nextVictim++ % pool.workerCount];
        if (auto *domain = victim.movable.last; &victim != &executor && domain) {
            victim.movable.remove(*domain);
            domain->executor = &executor;
            return domain;
        }
    }
    return nullptr;
}

void dispatch(uv_idle_t *idle) {
    TaskLock lock;
    auto& executor = *static_cast<Executor *>(idle->data);
    auto *domain = nextDomain(executor);
    if (!domain) {
        executor.sleeping = executor.worker;
        refresh(executor);
        return;
    }
    executor.stepping = true;
    domain->running = true;
    auto *task = domain->first;
    domain->first = task->nextReady;
    if (!domain->first) domain->last = nullptr;
    task->nextReady = nullptr;
    task->state = LYRA_TASK_RUNNING;
    if (task->joining) publish(lock, task);
    else {
        if (!executor.worker) ++executor.steps;
        if (task->cancelRequested && task->firstChild) {
            task->state = LYRA_TASK_SUSPENDED;
            task->resumeAfterChildren = true;
        } else {
            auto *previous = currentTask;
            currentTask = task;
            lock.unlock();
            task->step(task, task->frame);
            currentTask = previous;
            lock.lock();
            require(task->state != LYRA_TASK_RUNNING, "step returned without completing or suspending");
        }
    }
    domain->running = false;
    executor.stepping = false;
    settleDomain(*domain);
    if (!executor.worker) refresh(executor);
}

void enqueue(LyraTask *task) {
    auto& domain = *task->domain;
    task->state = LYRA_TASK_READY;
    if (domain.last) domain.last->nextReady = task;
    else domain.first = task;
    domain.last = task;
    schedule(domain);
}

void detachChoices(WaitSet& set) {
    for (size_t index = 0; index < set.count; ++index) {
        auto& choice = set.choices[index];
        auto *queue = choice.queue;
        if (!queue) continue;
        if (choice.previous) choice.previous->next = choice.next;
        else queue->first = choice.next;
        if (choice.next) choice.next->previous = choice.previous;
        else queue->last = choice.previous;
        choice.queue = nullptr;
        choice.previous = choice.next = nullptr;
    }
}

void commitChoice(WaitChoice& choice, void *result = nullptr, bool rejected = false) {
    auto& set = *choice.set;
    set.winner = &choice;
    set.result = result;
    set.rejected = rejected;
    detachChoices(set);
    if (set.task->state == LYRA_TASK_SUSPENDED) enqueue(set.task);
}

bool cancelled(LyraTask *task) {
    if (!task->cancelRequested && lyraContextRemaining(task->context) == 0) requestCancel(task);
    return task->cancelRequested;
}

bool takeTimer(WaitSet& set, const WaitChoice *limit, int64_t now) {
    for (auto *choice = set.choices; choice != limit; ++choice) {
        if (choice->kind == WaitKind::Timer && choice->time <= now) {
            commitChoice(*choice);
            return true;
        }
    }
    return false;
}

bool eligible(WaitChoice& choice, int64_t now) {
    return !cancelled(choice.set->task) && !takeTimer(*choice.set, &choice, now);
}

WaitChoice *peer(ChannelQueue& queue, int64_t now) {
    while (auto *candidate = queue.first) if (eligible(*candidate, now)) return candidate;
    return nullptr;
}

void pushMessage(LyraChannel& channel, void *value) {
    if (channel.count == channel.allocated) {
        auto slots = std::min(channel.capacity, std::max<size_t>(4, channel.allocated * 2));
        auto **items = static_cast<void **>(lyraGcAlloc(slots * sizeof(void *)));
        for (size_t index = 0; index < channel.count; ++index) items[index] = channel.items[(channel.head + index) % channel.allocated];
        channel.items = items;
        channel.allocated = slots;
        channel.head = 0;
    }
    channel.items[(channel.head + channel.count++) % channel.allocated] = value;
}

void *popMessage(LyraChannel& channel) {
    auto *value = channel.items[channel.head];
    channel.items[channel.head] = nullptr;
    channel.head = (channel.head + 1) % channel.allocated;
    --channel.count;
    return value;
}

bool tryChoice(WaitChoice& choice, int64_t now) {
    if (choice.kind == WaitKind::Timer) {
        if (choice.time > now) return false;
        commitChoice(choice);
        return true;
    }
    auto& channel = *choice.channel;
    if (choice.kind == WaitKind::Send) {
        if (channel.closed) commitChoice(choice, nullptr, true);
        else if (auto *receiver = peer(channel.receivers, now)) {
            commitChoice(*receiver, choice.boxed);
            commitChoice(choice);
        } else if (channel.count < channel.capacity) {
            pushMessage(channel, choice.boxed);
            commitChoice(choice);
        } else return false;
    } else {
        if (channel.count) {
            auto *value = popMessage(channel);
            if (!channel.closed) if (auto *sender = peer(channel.senders, now)) {
                pushMessage(channel, sender->boxed);
                commitChoice(*sender);
            }
            commitChoice(choice, value);
        } else if (channel.closed) commitChoice(choice);
        else if (auto *sender = peer(channel.senders, now)) {
            commitChoice(choice, sender->boxed);
            commitChoice(*sender);
        } else return false;
    }
    return true;
}

void attachChoice(WaitChoice& choice) {
    auto& channel = *choice.channel;
    auto& queue = choice.kind == WaitKind::Send ? channel.senders : channel.receivers;
    choice.queue = &queue;
    choice.ticket = channel.nextTicket++;
    choice.previous = queue.last;
    if (queue.last) queue.last->next = &choice;
    else queue.first = &choice;
    queue.last = &choice;
}

void timerReady(uv_timer_t *timer) {
    TaskLock lock;
    auto& set = *static_cast<WaitTimer *>(timer->data)->set;
    if (set.winner || cancelled(set.task)) return;
    auto now = lyraContextClock();
    if (!takeTimer(set, set.choices + set.count, now))
        require(uv_timer_start(timer, timerReady, static_cast<uint64_t>(set.deadline - now), 0) == 0, "cannot reschedule timer");
}

void startTimer(WaitSet& set) {
    auto *timer = allocateRuntime<WaitTimer>("cannot allocate operation timer");
    auto *domain = set.task->domain;
    auto& loop = domain->executor->loop;
    require(uv_timer_init(&loop, &timer->handle) == 0, "cannot initialize operation timer");
    timer->handle.data = timer;
    timer->domain = domain;
    timer->set = &set;
    set.timer = timer;
    uv_update_time(&loop);
    auto delay = std::max<int64_t>(0, set.deadline - lyraContextClock());
    require(uv_timer_start(&timer->handle, timerReady, static_cast<uint64_t>(delay), 0) == 0, "cannot schedule operation timer");
    ++domain->loopUsers;
}

void stopTimer(WaitTimer *timer) {
    uv_timer_stop(&timer->handle);
    uv_close(reinterpret_cast<uv_handle_t *>(&timer->handle), [](uv_handle_t *handle) {
        auto *closing = static_cast<WaitTimer *>(handle->data);
        auto *domain = closing->domain;
        free(closing);
        releaseWatchPin(*domain);
    });
}

void finish(TaskLock& lock, LyraTask *task, LyraTaskState state, void *outcome);

void waitStep(LyraTask *task, void *storage) {
    TaskLock lock;
    auto& set = *static_cast<WaitSet *>(storage);
    if (!set.winner && !cancelled(task)) {
        auto now = lyraContextClock();
        set.deadline = -1;
        for (size_t index = 0; index < set.count; ++index) {
            auto& choice = set.choices[index];
            if (choice.kind != WaitKind::Timer) continue;
            choice.time = now + std::clamp<int64_t>(choice.time, 0, INT64_MAX - now);
            if (set.deadline < 0 || choice.time < set.deadline) set.deadline = choice.time;
        }
        for (size_t index = 0; index < set.count; ++index) if (tryChoice(set.choices[index], now)) break;
        if (!set.winner) {
            for (size_t index = 0; index < set.count; ++index) if (set.choices[index].channel) attachChoice(set.choices[index]);
            task->state = LYRA_TASK_SUSPENDED;
            if (set.deadline >= 0) startTimer(set);
            return;
        }
    }
    if (set.timer) stopTimer(set.timer);
    task->wait = nullptr;
    if (!set.winner) { finish(lock, task, LYRA_TASK_CANCELLED, nullptr); return; }
    const auto& choice = *set.winner;
    if (set.rejected) {
        constexpr char message[] = "send on closed channel";
        auto *error = lyraExceptionCreateTyped(lyraStringFromBytes(message, sizeof(message) - 1), choice.channel->closedType, "ChannelClosed");
        const auto *source = choice.source ? choice.source : task;
        task->error = source->createdFile ? lyraExceptionRaise(error, source->createdFile, source->createdLine, source->createdColumn) : error;
        finish(lock, task, LYRA_TASK_FAILED, nullptr);
        return;
    }
    static unsigned char unit{};
    void *result = choice.pack ? choice.pack(set.result) : &unit;
    if (set.selecting) result = ::new (lyraGcAlloc(sizeof(LyraSelection))) LyraSelection{
        static_cast<uint64_t>(set.winner - set.choices), result};
    finish(lock, task, LYRA_TASK_COMPLETED, result);
}

WaitSet& makeWait(size_t count, bool selecting) {
    auto *set = ::new (lyraGcAlloc(sizeof(WaitSet))) WaitSet{};
    set->count = count;
    set->selecting = selecting;
    set->choices = static_cast<WaitChoice *>(lyraGcAlloc(count * sizeof(WaitChoice)));
    set->task = lyraTaskCreate(waitStep, set);
    set->task->wait = set;
    return *set;
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
    auto *previousTask = blockingTask;
    workerContext = request->context;
    blockingTask = request->task;
    request->work(request->nativeData);
    blockingTask = previousTask;
    workerContext = previous;
    lyraGcLeaveWorker(registration);
}

void finishBlocking(uv_work_t *work, int status) {
    require(status == 0 || status == UV_ECANCELED, "native work did not finish");
    auto *request = static_cast<BlockingRequest *>(work->data);
    auto *task = request->task;
    TaskLock lock;
    task->blocking = nullptr;
    lyraGcRootDrop(request->payloadRoot);
    free(request);
    --task->domain->loopUsers;
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
    auto *handle = reinterpret_cast<uv_handle_t *>(&watch->ready);
#else
    uv_poll_stop(&watch->poll);
    auto *handle = reinterpret_cast<uv_handle_t *>(&watch->poll);
#endif
    uv_close(handle, [](uv_handle_t *handle) {
        auto *closing = static_cast<DisconnectWatch *>(handle->data);
#ifndef _WIN32
        close(closing->fd);
#endif
        if (closing->target) lyraGcRootDrop(closing->target);
        auto *domain = closing->domain;
        free(closing);
        releaseWatchPin(*domain);
    });
}

void disconnectObserved(DisconnectWatch *watch) {
    TaskLock lock;
    auto **link = &watch->task->watches;
    while (*link != watch) link = &(*link)->next;
    *link = watch->next;
    if (!watch->locallyClosed(lyraGcRootGet(watch->target))) requestCancel(watch->task);
    closeWatch(watch);
}

void stopDeadline(LyraTask *task) {
    if (auto *watch = task->deadlineWatch) {
        task->deadlineWatch = nullptr;
        uv_timer_stop(&watch->timer);
        uv_close(reinterpret_cast<uv_handle_t *>(&watch->timer), [](uv_handle_t *handle) {
            auto *closing = static_cast<DeadlineWatch *>(handle->data);
            auto *domain = closing->domain;
            free(closing);
            releaseWatchPin(*domain);
        });
    }
}

void armDeadline(LyraTask *task, int64_t remaining) {
    if (remaining == 0) { requestCancel(task); return; }
    auto *watch = static_cast<DeadlineWatch *>(calloc(1, sizeof(DeadlineWatch)));
    require(watch != nullptr, "cannot allocate deadline timer");
    auto& loop = task->domain->executor->loop;
    require(uv_timer_init(&loop, &watch->timer) == 0, "cannot initialize deadline timer");
    watch->task = task;
    watch->domain = task->domain;
    watch->timer.data = watch;
    task->deadlineWatch = watch;
    uv_update_time(&loop);
    require(uv_timer_start(&watch->timer, [](uv_timer_t *timer) {
        lyraTaskRequestCancel(static_cast<DeadlineWatch *>(timer->data)->task);
    }, static_cast<uint64_t>(remaining), 0) == 0, "cannot schedule deadline");
    ++task->domain->loopUsers;
}

bool closesWaitCycle(LyraTask *waiter, LyraTask *target = nullptr) {
    if (target == waiter) return true;
    if (target && !target->waitingOn && !target->joining && !target->resumeAfterChildren) return false;
    LyraTask *first = nullptr, *last = nullptr;
    auto visit = [&](LyraTask *task) {
        if (!task || task->nextWaitSearch) return;
        if (last) last->nextWaitSearch = task;
        else first = task;
        last = task;
        task->nextWaitSearch = task;
    };
    auto visitChildren = [&](LyraTask *task) {
        for (auto *child = task->firstChild; child; child = child->nextChild) visit(child);
    };
    if (target) visit(target);
    else visitChildren(waiter);
    auto *current = first;
    while (current && current != waiter) {
        visit(current->waitingOn);
        if (current->joining || current->resumeAfterChildren) visitChildren(current);
        current = current == last ? nullptr : current->nextWaitSearch;
    }
    for (auto *task = first; task;) {
        auto *next = task == last ? nullptr : task->nextWaitSearch;
        task->nextWaitSearch = nullptr;
        task = next;
    }
    return current == waiter;
}

const LyraException *cycleError(LyraTask *task, const char *message) {
    return lyraExceptionCreateTyped(lyraStringFromBytes(message, strlen(message)), task->domain->cycleErrorType, "TaskCycleError");
}

void finish(TaskLock& lock, LyraTask *task, LyraTaskState state, void *outcome) {
    require(task->state == LYRA_TASK_RUNNING, "only a running task can finish");
    if (state == LYRA_TASK_COMPLETED && task->firstChild && closesWaitCycle(task)) {
        task->error = cycleError(task, "joining child tasks would create a task wait cycle");
        state = LYRA_TASK_FAILED;
        outcome = nullptr;
    }
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
    auto *primary = task->error;
    for (auto *cause = task->failures; cause; cause = cause->next) primary = lyraExceptionAddTaskFailure(primary, cause->cause);
    for (auto *cause = task->cancelledCleanups; cause; cause = cause->next) primary = lyraExceptionAddCleanupFailure(primary, cause->cause);
    if (auto **stack = task->cleanups) {
        task->cleanups = nullptr;
        auto *previous = currentTask;
        currentTask = task;
        lock.unlock();
        primary = lyraCleanupUntil(stack, nullptr, primary);
        lock.lock();
        currentTask = previous;
    }
    auto terminal = task->pendingState;
    if (terminal == LYRA_TASK_COMPLETED && task->cancelRequested) terminal = LYRA_TASK_CANCELLED;
    if (primary) {
        if (terminal != LYRA_TASK_CANCELLED) terminal = LYRA_TASK_FAILED;
        task->error = task->createdFile ? lyraExceptionFrame(primary, "created", task->createdFile, task->createdLine, task->createdColumn) : primary;
    }
    if (terminal != LYRA_TASK_COMPLETED) task->outcome = nullptr;
    task->state = terminal;
    task->failures = task->cancelledCleanups = nullptr;
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
        } else if (task->state == LYRA_TASK_CANCELLED && task->error && (parent->cancelRequested || parent->failures || (parent->joining && parent->pendingState == LYRA_TASK_FAILED))) {
            addFailure(parent->cancelledCleanups, task->ordinal + 1, task->error);
        }
        if (!parent->firstChild && (parent->joining || parent->resumeAfterChildren)) {
            parent->resumeAfterChildren = false;
            enqueue(parent);
        }
    }
    task->domain = nullptr;
    --domain->live;
    auto *root = task->activeRoot;
    task->activeRoot = nullptr;
    lyraGcRootDrop(root);
}

void requestCancel(LyraTask *task) {
    auto *current = task;
    for (;;) {
        if (!isTerminal(current) && !current->cancelRequested && !(current->wait && current->wait->winner)) {
            current->cancelRequested = true;
            if (current->joining && current->pendingState == LYRA_TASK_COMPLETED) current->pendingState = LYRA_TASK_CANCELLED;
            if (current->blocking) uv_cancel(reinterpret_cast<uv_req_t *>(&current->blocking->request));
            if (current->state == LYRA_TASK_SUSPENDED && !current->joining && !current->blocking) {
                if (current->waitingOn) detachWaiter(current);
                if (current->wait) detachChoices(*current->wait);
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

LyraAwaitStatus awaitTask(LyraTask *waiter, LyraTask *target) {
    require(waiter->state == LYRA_TASK_RUNNING, "only a running task can await");
    if (waiter->cancelRequested) { enqueue(waiter); return LYRA_AWAIT_PENDING; }
    if (target->state == LYRA_TASK_COMPLETED) return LYRA_AWAIT_VALUE;
    if (target->state == LYRA_TASK_FAILED) return LYRA_AWAIT_EXCEPTION;
    if (target->state == LYRA_TASK_CANCELLED) return LYRA_AWAIT_CANCELLED;
    if (closesWaitCycle(waiter, target)) return LYRA_AWAIT_CYCLE;
    startChild(waiter, target, false, waiter->domain);
    if (target->owner == waiter) target->reportToGroup = false;
    waiter->nextWaiter = target->firstWaiter;
    target->firstWaiter = waiter;
    waiter->waitingOn = target;
    waiter->state = LYRA_TASK_SUSPENDED;
    transferLease(waiter->domain, target->domain, true);
    return LYRA_AWAIT_PENDING;
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
    const LyraException *error;
    switch (lyraTaskAwaitValue(self, scope->body, &error)) {
        case LYRA_AWAIT_PENDING: return;
        case LYRA_AWAIT_VALUE: lyraTaskComplete(self, lyraTaskOutcome(scope->body)); return;
        case LYRA_AWAIT_EXCEPTION: lyraTaskFail(self, error); return;
        case LYRA_AWAIT_CANCELLED: case LYRA_AWAIT_CYCLE: require(false, "unexpected ordinary await status"); return;
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

int64_t lyraTaskParallelism() { return static_cast<int64_t>(parallelWorkerCount()); }

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
        if (executor->pool) for (size_t index = 0; index < executor->pool->workerCount; ++index) {
            auto& worker = executor->pool->workers[index];
            worker.stopping = true;
            require(uv_async_send(&worker.inbox) == 0, "cannot stop parallel executor");
        }
        closeExecutor(*executor);
    }
    if (auto *pool = executor->pool) {
        for (size_t index = 0; index < pool->workerCount; ++index)
            require(uv_thread_join(&pool->workers[index].thread) == 0, "cannot join parallel executor");
        free(pool->workers);
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

LyraChannel *lyraChannelCreate(int64_t capacity, const void *closedType, const LyraException **error) {
    if (capacity < 0 || static_cast<uint64_t>(capacity) > SIZE_MAX / sizeof(void *)) {
        constexpr char message[] = "Channel capacity is outside the supported range";
        *error = lyraExceptionCreate(lyraStringFromBytes(message, sizeof(message) - 1));
        return nullptr;
    }
    auto *channel = ::new (lyraGcAlloc(sizeof(LyraChannel))) LyraChannel{};
    channel->capacity = static_cast<size_t>(capacity);
    channel->closedType = closedType;
    return channel;
}

LyraTask *lyraChannelSend(LyraChannel *channel, void *boxed) {
    auto& set = makeWait(1, false);
    set.choices[0] = {.kind = WaitKind::Send, .channel = channel, .boxed = boxed, .set = &set};
    return set.task;
}

LyraTask *lyraChannelReceive(LyraChannel *channel, LyraChannelPack pack) {
    auto& set = makeWait(1, false);
    set.choices[0] = {.kind = WaitKind::Receive, .channel = channel, .pack = pack, .set = &set};
    return set.task;
}

void lyraChannelClose(LyraChannel *channel) {
    TaskLock lock;
    if (channel->closed) return;
    channel->closed = true;
    auto now = lyraContextClock();
    while (channel->senders.first || channel->receivers.first) {
        auto *sender = channel->senders.first;
        auto *receiver = channel->receivers.first;
        if (sender && (!receiver || sender->ticket < receiver->ticket)) {
            if (eligible(*sender, now)) commitChoice(*sender, nullptr, true);
        } else if (eligible(*receiver, now)) commitChoice(*receiver, channel->count ? popMessage(*channel) : nullptr);
    }
}

LyraTask *lyraTaskSleep(int64_t milliseconds) {
    auto& set = makeWait(1, false);
    set.choices[0] = {.kind = WaitKind::Timer, .time = milliseconds, .set = &set};
    return set.task;
}

LyraTask *lyraTaskSelect(size_t arms) { return makeWait(arms, true).task; }

void lyraTaskSelectArm(LyraTask *select, size_t index, const LyraTask *operation) {
    auto& choice = select->wait->choices[index];
    choice = operation->wait->choices[0];
    choice.set = select->wait;
    choice.source = operation;
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

int lyraTaskWatchDisconnect(uintptr_t monitor, void *target, int (*locallyClosed)(const void *)) {
    if (!currentTask || !monitor) return 0;
    TaskLock lock;
    auto *watch = static_cast<DisconnectWatch *>(calloc(1, sizeof(DisconnectWatch)));
    require(watch != nullptr, "cannot allocate disconnect watch");
    watch->task = currentTask;
    watch->domain = currentTask->domain;
    auto *loop = &watch->domain->executor->loop;
#ifdef _WIN32
    auto *process = GetCurrentProcess();
    if (!DuplicateHandle(process, reinterpret_cast<HANDLE>(monitor), process, &watch->event, 0, FALSE, DUPLICATE_SAME_ACCESS)) { free(watch); return 0; }
    if (uv_async_init(loop, &watch->ready, [](uv_async_t *handle) {
        disconnectObserved(static_cast<DisconnectWatch *>(handle->data));
    })) { CloseHandle(watch->event); free(watch); return 0; }
    watch->ready.data = watch;
    ++watch->domain->loopUsers;
    if (!RegisterWaitForSingleObject(&watch->waiter, watch->event, [](void *data, BOOLEAN) {
        uv_async_send(&static_cast<DisconnectWatch *>(data)->ready);
    }, watch, INFINITE, WT_EXECUTEONLYONCE)) { closeWatch(watch); return 0; }
#else
    watch->fd = fcntl(static_cast<int>(monitor - 1), F_DUPFD_CLOEXEC, 0);
    if (watch->fd < 0) { free(watch); return 0; }
    if (uv_poll_init_socket(loop, &watch->poll, watch->fd)) { close(watch->fd); free(watch); return 0; }
    watch->poll.data = watch;
    ++watch->domain->loopUsers;
    int status = uv_poll_start(&watch->poll, UV_DISCONNECT, [](uv_poll_t *poll, int status, int events) {
        if (status < 0 || (events & UV_DISCONNECT)) disconnectObserved(static_cast<DisconnectWatch *>(poll->data));
    });
    if (status) { closeWatch(watch); return 0; }
#endif
    watch->target = lyraGcRootCreate(target);
    watch->locallyClosed = locallyClosed;
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
    auto next = [&]() -> Executor* { return &pool.workers[pool.next++ % pool.workerCount]; };
    auto *executor = next();
    if (executor == &host && pool.workerCount > 1) executor = next();
    auto *domain = allocateRuntime<LyraDomain>("cannot allocate parallel execution domain");
    domain->executor = executor;
    domain->cycleErrorType = parent->domain->cycleErrorType;
    domain->pooled = true;
    startChild(parent, target, true, domain);
    return target;
}

int lyraTaskRunRoot(LyraTask *task, const void *cycleErrorType) {
    auto *domain = lyraDomainCreate();
    domain->cycleErrorType = cycleErrorType;
    lyraTaskStart(domain, task);
    lyraDomainRun(domain);
    require(domain->live == 0, "no runnable work can complete the root task group");
    int status = task->state == LYRA_TASK_COMPLETED ? EXIT_SUCCESS : EXIT_FAILURE;
    if (task->state == LYRA_TASK_CANCELLED) fputs("Lyra runtime: async entry was cancelled\n", stderr);
    if (task->error) lyraExceptionReport(task->error);
    lyraDomainDestroy(domain);
    return status;
}

LyraAwaitStatus lyraTaskAwaitValue(LyraTask *waiter, LyraTask *target, const LyraException **error) {
    TaskLock lock;
    *error = nullptr;
    auto status = awaitTask(waiter, target);
    if (status == LYRA_AWAIT_CYCLE) {
        *error = cycleError(waiter, "await would create a task wait cycle");
        return LYRA_AWAIT_EXCEPTION;
    }
    if (status == LYRA_AWAIT_EXCEPTION) *error = target->error;
    if (status == LYRA_AWAIT_CANCELLED) {
        if (waiter->failures && !waiter->cancelRequested) {
            finish(lock, waiter, LYRA_TASK_FAILED, nullptr);
            return LYRA_AWAIT_PENDING;
        }
        waiter->error = target->error;
        requestCancel(waiter);
        finish(lock, waiter, LYRA_TASK_CANCELLED, nullptr);
        return LYRA_AWAIT_PENDING;
    }
    return status;
}

LyraAwaitStatus lyraTaskAwaitOutcome(LyraTask *waiter, LyraTask *target, const LyraException **error) {
    TaskLock lock;
    cancelled(waiter);
    auto status = awaitTask(waiter, target);
    *error = status == LYRA_AWAIT_CYCLE ? cycleError(waiter, "awaitOutcome would create a task wait cycle") : nullptr;
    return status;
}

void lyraTaskSuspend(LyraTask *task) {
    TaskLock lock;
    require(task->state == LYRA_TASK_RUNNING, "only a running task can suspend");
    task->state = LYRA_TASK_SUSPENDED;
    if (task->cancelRequested) enqueue(task);
}

void lyraTaskWake(LyraTask *task) {
    TaskLock lock;
    if (task->state == LYRA_TASK_SUSPENDED && !task->waitingOn && !task->wait && !task->blocking && !task->joining && !task->resumeAfterChildren) enqueue(task);
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
    task->blocking = request;
    ++task->domain->loopUsers;
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

int lyraTaskCancelRequested(LyraTask *task) {
    TaskLock lock;
    return cancelled(task);
}

int lyraBlockingCancelled() { return blockingTask && lyraTaskCancelRequested(blockingTask); }

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

LyraList *lyraTaskCancellationFailures(const LyraTask *task) {
    TaskLock lock;
    require(task->state == LYRA_TASK_CANCELLED, "task has not finished cancellation");
    auto *error = task->error;
    lock.unlock();
    return error ? lyraExceptionCleanupFailures(error) : lyraListCreate();
}
