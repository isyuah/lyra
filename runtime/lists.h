#ifndef LYRA_RUNTIME_LISTS_H
#define LYRA_RUNTIME_LISTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LyraList LyraList;
LyraList* lyraListCreate(void);
int64_t lyraListSize(const LyraList* list);
LyraList* lyraListSlice(const LyraList* list, int64_t start);
LyraList* lyraListSliceRange(const LyraList* list, int64_t start, int64_t end, uint8_t inclusive, const char* file, uint64_t line, uint64_t column);
const void* lyraListGet(const LyraList* list, int64_t index, const char* file, uint64_t line, uint64_t column);
const void* lyraFrozenListItem(const LyraList* list, uint64_t index);
void lyraListSet(LyraList* list, int64_t index, const void* boxed);
void lyraListAdd(LyraList* list, const void* boxed);
void lyraListExtend(LyraList* list, const LyraList* source);
void lyraListExtendAny(LyraList* list, const LyraList* source, const void* type);

#ifdef __cplusplus
}
#endif

#endif
