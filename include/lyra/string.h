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

#ifdef __cplusplus
}
#endif

#endif
