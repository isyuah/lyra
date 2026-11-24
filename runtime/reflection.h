#ifndef LYRA_RUNTIME_REFLECTION_H
#define LYRA_RUNTIME_REFLECTION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LyraReflectType LyraReflectType;
typedef struct LyraReflectField LyraReflectField;

struct LyraReflectField {
    const char* name;
    const void* type;
    void (*get)(const void* value, void* output);
};

struct LyraReflectType {
    uint32_t abiVersion;
    uint32_t kind;
    const char* name;
    int64_t fieldCount;
    const LyraReflectField* fields;
};

typedef struct LyraAnyValue {
    const void* type;
    const void* data;
} LyraAnyValue;

const void* lyraReflectTypeName(const void* type);
int64_t lyraReflectFieldCount(const void* type);
const void* lyraReflectFieldNames(const void* type);
uint8_t lyraReflectFieldValue(const void* value, const void* name, void* output);

#ifdef __cplusplus
}
#endif

#endif
