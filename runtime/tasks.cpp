#include "tasks.h"
#include "managed.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <uv.h>

struct LyraDomain {
    uv_loop_t loop;
    uv_idle_t idle;
    LyraTask *first;
    LyraTask *last;
    size_t live;
    size_t steps;
};

struct LyraTask {
    LyraTaskStep step;
    void *frame;
    void *outcome;
    LyraDomain *domain;
    LyraGcRoot *activeRoot;
    LyraTask *nextReady;
    LyraTask *firstWaiter;
    LyraTask *nextWaiter;
    LyraTask *waitingOn;
    LyraTaskState state;
    bool cancelRequested;
    bool blocking;
};

namespace {

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
    task->step(task, task->frame);
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
};

void runBlocking(uv_work_t *work) {
    auto *request = static_cast<BlockingRequest *>(work->data);
    auto registration = lyraGcEnterWorker();
    request->work(request->nativeData);
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

void finish(LyraTask *task, LyraTaskState state, void *outcome) {
    require(task->state == LYRA_TASK_RUNNING, "only a running task can finish");
    task->outcome = outcome;
    task->state = state;
    task->frame = nullptr;
    task->step = nullptr;
    while (auto *waiter = task->firstWaiter) {
        detachWaiter(waiter);
        enqueue(waiter);
    }
    --task->domain->live;
    task->domain = nullptr;
    auto *root = task->activeRoot;
    task->activeRoot = nullptr;
    lyraGcRootDrop(root);
}

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

void lyraTaskStart(LyraDomain *domain, LyraTask *task) {
    if (task->state != LYRA_TASK_COLD) return;
    task->activeRoot = lyraGcRootCreate(task);
    task->domain = domain;
    ++domain->live;
    enqueue(task);
}

int lyraTaskAwait(LyraTask *waiter, LyraTask *target) {
    require(waiter->state == LYRA_TASK_RUNNING && waiter != target, "invalid task wait");
    if (waiter->cancelRequested) {
        enqueue(waiter);
        return 0;
    }
    if (isTerminal(target)) return 1;
    lyraTaskStart(waiter->domain, target);
    waiter->nextWaiter = target->firstWaiter;
    target->firstWaiter = waiter;
    waiter->waitingOn = target;
    waiter->state = LYRA_TASK_SUSPENDED;
    return 0;
}

void lyraTaskSuspend(LyraTask *task) {
    require(task->state == LYRA_TASK_RUNNING, "only a running task can suspend");
    task->state = LYRA_TASK_SUSPENDED;
    if (task->cancelRequested) enqueue(task);
}

void lyraTaskWake(LyraTask *task) {
    if (task->state == LYRA_TASK_SUSPENDED && !task->waitingOn && !task->blocking) enqueue(task);
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
    request->request.data = request;
    task->state = LYRA_TASK_SUSPENDED;
    task->blocking = true;
    require(uv_queue_work(&task->domain->loop, &request->request, runBlocking, finishBlocking) == 0, "cannot schedule native work");
}

void lyraTaskComplete(LyraTask *task, void *value) {
    finish(task, LYRA_TASK_COMPLETED, value);
}

void lyraTaskFail(LyraTask *task, void *error) {
    finish(task, LYRA_TASK_FAILED, error);
}

void lyraTaskRequestCancel(LyraTask *task) {
    if (isTerminal(task)) return;
    task->cancelRequested = true;
    if (task->state == LYRA_TASK_SUSPENDED && !task->blocking) {
        if (task->waitingOn) detachWaiter(task);
        enqueue(task);
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
    require(isTerminal(task), "task does not have an outcome yet");
    return task->outcome;
}
