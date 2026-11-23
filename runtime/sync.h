#ifndef LYRA_RUNTIME_SYNC_H
#define LYRA_RUNTIME_SYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LyraMutex LyraMutex;
typedef struct LyraAtomicInt LyraAtomicInt;

LyraMutex *lyraMutexCreate(void);
void lyraMutexLock(LyraMutex *mutex);
void lyraMutexUnlock(LyraMutex *mutex);

LyraAtomicInt *lyraAtomicIntCreate(int64_t value);
int64_t lyraAtomicIntLoad(const LyraAtomicInt *number);
void lyraAtomicIntStore(LyraAtomicInt *number, int64_t value);
int64_t lyraAtomicIntFetchAdd(LyraAtomicInt *number, int64_t amount);
uint8_t lyraAtomicIntCompareExchange(LyraAtomicInt *number, int64_t expected, int64_t replacement);

#ifdef __cplusplus
}
#endif

#endif
