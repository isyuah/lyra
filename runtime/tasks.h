#ifndef LYRA_RUNTIME_TASKS_H
#define LYRA_RUNTIME_TASKS_H

#include <stddef.h>
#include "managed.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LyraDomain LyraDomain;
typedef struct LyraTask LyraTask;
typedef struct LyraChannel LyraChannel;
typedef struct LyraList LyraList;
typedef void (*LyraTaskStep)(LyraTask *task, void *frame);
typedef void (*LyraNativeWork)(void *nativeData);
typedef void *(*LyraBlockingBody)(void *environment, const LyraException **error);
typedef void *(*LyraChannelPack)(void *boxed);
typedef struct LyraSelection { uint64_t arm; void *outcome; } LyraSelection;

typedef enum LyraTaskState {
    LYRA_TASK_COLD, LYRA_TASK_READY, LYRA_TASK_RUNNING, LYRA_TASK_SUSPENDED,
    LYRA_TASK_COMPLETED, LYRA_TASK_FAILED, LYRA_TASK_CANCELLED
} LyraTaskState;

typedef enum LyraAwaitStatus { LYRA_AWAIT_PENDING, LYRA_AWAIT_VALUE, LYRA_AWAIT_EXCEPTION, LYRA_AWAIT_CANCELLED, LYRA_AWAIT_CYCLE } LyraAwaitStatus;

LyraDomain *lyraDomainCreate(void);
int64_t lyraTaskParallelism(void);
size_t lyraDomainRun(LyraDomain *domain);
void lyraDomainDestroy(LyraDomain *domain);

LyraTask *lyraTaskCreate(LyraTaskStep step, void *frame);
void lyraTaskCreatedAt(LyraTask *task, const char *file, uint64_t line, uint64_t column);
void lyraTaskKeepCleanups(LyraTask *task, LyraCleanup **stack);
LyraTask *lyraTaskBlocking(LyraBlockingBody body, void *environment);
LyraTask *lyraTaskWithLocal(const void *key, void *boxed, LyraTask *body);
LyraChannel *lyraChannelCreate(int64_t capacity, const void *closedType, const LyraException **error);
LyraTask *lyraChannelSend(LyraChannel *channel, void *boxed);
LyraTask *lyraChannelReceive(LyraChannel *channel, LyraChannelPack pack);
void lyraChannelClose(LyraChannel *channel);
LyraTask *lyraTaskSleep(int64_t milliseconds);
LyraTask *lyraTaskSelect(size_t arms);
void lyraTaskSelectArm(LyraTask *select, size_t index, const LyraTask *operation);
void lyraTaskSharedCopy(void *destination, const void *source, size_t bytes);
void lyraTaskStart(LyraDomain *domain, LyraTask *task);
LyraAwaitStatus lyraTaskAwaitValue(LyraTask *waiter, LyraTask *target, const LyraException **error);
LyraAwaitStatus lyraTaskAwaitOutcome(LyraTask *waiter, LyraTask *target, const LyraException **error);
LyraTask *lyraTaskSpawn(LyraTask *parent, LyraTask *target);
LyraTask *lyraTaskParallel(LyraTask *parent, LyraTask *target);
int lyraTaskRunRoot(LyraTask *task, const void *cycleErrorType);
void lyraTaskSuspend(LyraTask *task);
void lyraTaskWake(LyraTask *task);
void lyraTaskBlock(LyraTask *task, LyraNativeWork work, void *nativeData);
void lyraTaskComplete(LyraTask *task, void *value);
void lyraTaskFail(LyraTask *task, const LyraException *error);
void lyraTaskRequestCancel(LyraTask *task);
int lyraTaskCancelRequested(LyraTask *task);
int lyraBlockingCancelled(void);
void lyraTaskFinishCancelled(LyraTask *task);
LyraTaskState lyraTaskState(const LyraTask *task);
void *lyraTaskOutcome(const LyraTask *task);
const LyraException *lyraTaskException(const LyraTask *task);
LyraList *lyraTaskCancellationFailures(const LyraTask *task);

#ifdef __cplusplus
}
#endif

#endif
