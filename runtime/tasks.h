#ifndef LYRA_RUNTIME_TASKS_H
#define LYRA_RUNTIME_TASKS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LyraDomain LyraDomain;
typedef struct LyraTask LyraTask;
typedef void (*LyraTaskStep)(LyraTask *task, void *frame);
typedef void (*LyraNativeWork)(void *nativeData);

typedef enum LyraTaskState {
    LYRA_TASK_COLD, LYRA_TASK_READY, LYRA_TASK_RUNNING, LYRA_TASK_SUSPENDED,
    LYRA_TASK_COMPLETED, LYRA_TASK_FAILED, LYRA_TASK_CANCELLED
} LyraTaskState;

LyraDomain *lyraDomainCreate(void);
size_t lyraDomainRun(LyraDomain *domain);
void lyraDomainDestroy(LyraDomain *domain);

LyraTask *lyraTaskCreate(LyraTaskStep step, void *frame);
void lyraTaskStart(LyraDomain *domain, LyraTask *task);
int lyraTaskAwait(LyraTask *waiter, LyraTask *target);
void lyraTaskSuspend(LyraTask *task);
void lyraTaskWake(LyraTask *task);
void lyraTaskBlock(LyraTask *task, LyraNativeWork work, void *nativeData);
void lyraTaskComplete(LyraTask *task, void *value);
void lyraTaskFail(LyraTask *task, void *error);
void lyraTaskRequestCancel(LyraTask *task);
int lyraTaskCancelRequested(const LyraTask *task);
void lyraTaskFinishCancelled(LyraTask *task);
LyraTaskState lyraTaskState(const LyraTask *task);
void *lyraTaskOutcome(const LyraTask *task);

#ifdef __cplusplus
}
#endif

#endif
