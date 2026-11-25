#ifndef LYRA_RUNTIME_IO_H
#define LYRA_RUNTIME_IO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LyraIoText LyraIoText;
typedef struct LyraIoBytes LyraIoBytes;
typedef struct LyraIoFile LyraIoFile;
typedef struct LyraIoFileOpen LyraIoFileOpen;
#ifdef _WIN32
void lyraIoSetArgs(int argc, wchar_t** argv);
#else
void lyraIoSetArgs(int argc, char** argv);
#endif
int64_t lyraIoArgCount(void);
LyraIoText* lyraIoArgument(int64_t index);
LyraIoText* lyraIoReadLine(int64_t maximum);
LyraIoText* lyraIoReadFile(const void* path);
int64_t lyraIoTextStatus(const LyraIoText* result);
uint8_t lyraIoInvalidUtf8(int64_t status);
const void* lyraIoTextValue(const LyraIoText* result);
int64_t lyraIoWriteFile(const void* path, const void* content);
LyraIoBytes* lyraIoReadBytes(const void* path);
int64_t lyraIoWriteBytes(const void* path, const void* content);
int64_t lyraIoBytesStatus(const LyraIoBytes* result);
const void* lyraIoBytesValue(const LyraIoBytes* result);
LyraIoFileOpen* lyraIoFileOpen(const void* path, int64_t mode);
int64_t lyraIoFileOpenStatus(const LyraIoFileOpen* result);
LyraIoFile* lyraIoFileOpened(const LyraIoFileOpen* result);
LyraIoBytes* lyraIoFileRead(LyraIoFile* file, int64_t maximum);
int64_t lyraIoFileWrite(LyraIoFile* file, const void* content);
int64_t lyraIoFileFlush(LyraIoFile* file);
int64_t lyraIoFileClose(LyraIoFile* file);
int64_t lyraIoWriteError(const void* content);

#ifdef __cplusplus
}
#endif

#endif
