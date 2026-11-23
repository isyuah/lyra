#ifndef LYRA_RUNTIME_MANAGED_H
#define LYRA_RUNTIME_MANAGED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LyraGcRoot LyraGcRoot;
typedef struct LyraCleanup LyraCleanup;
typedef void (*LyraCleanupCall)(void *environment);

void *lyraGcAlloc(size_t bytes);
void *lyraGcAllocAtomic(size_t bytes);
void lyraGcCollect(void);
LyraGcRoot *lyraGcRootCreate(void *value);
void *lyraGcRootGet(const LyraGcRoot *root);
void lyraGcRootDrop(LyraGcRoot *root);
void lyraCleanupPush(LyraCleanup **stack, LyraCleanupCall call, void *environment);
void lyraCleanupUntil(LyraCleanup **stack, const LyraCleanup *mark);
int lyraGcEnterWorker(void);
void lyraGcLeaveWorker(int registration);

#ifdef __cplusplus
}
#endif

#endif
