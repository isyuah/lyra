#ifndef LYRA_RUNTIME_CONTEXT_H
#define LYRA_RUNTIME_CONTEXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LyraContext LyraContext;
const LyraContext *lyraContextCurrent(void);
const LyraContext *lyraContextWithTrace(const LyraContext *context, const void *trace);
const LyraContext *lyraContextWithTimeout(const LyraContext *context, int64_t milliseconds);
const LyraContext *lyraContextWithLocal(const LyraContext *context, const void *key, void *value);
void *lyraContextLocal(const LyraContext *context, const void *key);
const void *lyraContextTrace(const LyraContext *context);
int64_t lyraContextDeadline(const LyraContext *context);
int64_t lyraContextRemaining(const LyraContext *context);
int64_t lyraContextClock(void);
uint8_t lyraContextEnter(const LyraContext *context);

#ifdef __cplusplus
}
#endif

#endif
