#ifndef LYRA_STRING_H
#define LYRA_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LyraString LyraString;

typedef struct LyraStringView {
    const char *data;
    size_t size;
} LyraStringView;

LyraStringView lyraStringView(const LyraString *value);

typedef struct LyraBytes LyraBytes;

typedef struct LyraBytesView {
    const unsigned char *data;
    size_t size;
} LyraBytesView;

LyraBytesView lyraBytesView(const LyraBytes *value);

#ifdef __cplusplus
}
#endif

#endif
