#ifndef LYRA_RUNTIME_TASKS_H
#define LYRA_RUNTIME_TASKS_H

#include <stddef.h>
#include "managed.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LyraDomain LyraDomain;
typedef struct LyraTask LyraTask;
typedef void (*LyraTaskStep)(LyraTask *task, void *frame);
typedef void (*LyraNativeWork)(void *nativeData);
typedef void *(*LyraBlockingBody)(void *environment, const LyraException **error);

typedef enum LyraTaskState {
    LYRA_TASK_COLD, LYRA_TASK_READY, LYRA_TASK_RUNNING, LYRA_TASK_SUSPENDED,
    LYRA_TASK_COMPLETED, LYRA_TASK_FAILED, LYRA_TASK_CANCELLED
} LyraTaskState;

typedef enum LyraAwaitStatus { LYRA_AWAIT_PENDING, LYRA_AWAIT_VALUE, LYRA_AWAIT_EXCEPTION } LyraAwaitStatus;

LyraDomain *lyraDomainCreate(void);
int64_t lyraTaskParallelism(void);
size_t lyraDomainRun(LyraDomain *domain);
void lyraDomainDestroy(LyraDomain *domain);

LyraTask *lyraTaskCreate(LyraTaskStep step, void *frame);
void lyraTaskCreatedAt(LyraTask *task, const char *file, uint64_t line, uint64_t column);
void lyraTaskKeepCleanups(LyraTask *task, LyraCleanup **stack);
LyraTask *lyraTaskBlocking(LyraBlockingBody body, void *environment);
LyraTask *lyraTaskWithLocal(const void *key, void *boxed, LyraTask *body);
void lyraTaskSharedCopy(void *destination, const void *source, size_t bytes);
int lyraTaskWatchDisconnect(uintptr_t monitor);
void lyraTaskStart(LyraDomain *domain, LyraTask *task);
int lyraTaskAwait(LyraTask *waiter, LyraTask *target);
LyraAwaitStatus lyraTaskAwaitValue(LyraTask *waiter, LyraTask *target);
LyraTask *lyraTaskSpawn(LyraTask *parent, LyraTask *target);
LyraTask *lyraTaskParallel(LyraTask *parent, LyraTask *target);
int lyraTaskRunRoot(LyraTask *task);
void lyraTaskSuspend(LyraTask *task);
void lyraTaskWake(LyraTask *task);
void lyraTaskBlock(LyraTask *task, LyraNativeWork work, void *nativeData);
void lyraTaskComplete(LyraTask *task, void *value);
void lyraTaskFail(LyraTask *task, const LyraException *error);
void lyraTaskRequestCancel(LyraTask *task);
int lyraTaskCancelRequested(const LyraTask *task);
void lyraTaskFinishCancelled(LyraTask *task);
LyraTaskState lyraTaskState(const LyraTask *task);
void *lyraTaskOutcome(const LyraTask *task);
const LyraException *lyraTaskException(const LyraTask *task);
size_t lyraTaskFailureCount(const LyraTask *task);
const LyraException *lyraTaskFailureAt(const LyraTask *task, size_t index);

#ifdef __cplusplus
}
#endif

#endif
