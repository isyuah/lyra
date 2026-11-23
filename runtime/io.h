#ifndef LYRA_RUNTIME_IO_H
#define LYRA_RUNTIME_IO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LyraIoText LyraIoText;
void lyraIoSetArgs(int argc, char** argv);
int64_t lyraIoArgCount(void);
LyraIoText* lyraIoArgument(int64_t index);
LyraIoText* lyraIoReadLine(void);
LyraIoText* lyraIoReadFile(const void* path);
int64_t lyraIoTextStatus(const LyraIoText* result);
const void* lyraIoTextValue(const LyraIoText* result);
int64_t lyraIoWriteFile(const void* path, const void* content);
int64_t lyraIoWriteError(const void* content);
void lyraIoAssert(uint8_t condition, const void* message);

#ifdef __cplusplus
}
#endif

#endif
