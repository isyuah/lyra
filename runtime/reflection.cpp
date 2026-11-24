#include "reflection.h"
#include "lists.h"
#include "managed.h"
#include "strings.h"

#include <cstring>
#include <cstdint>

namespace {

const LyraReflectType* descriptor(const void* token) {
    return static_cast<const LyraReflectType*>(token);
}

bool equals(const void* value, const char* name) {
    if (!value || !name) return false;
    const auto length = lyraStringLength(value);
    const auto nameLength = std::strlen(name);
    return length == nameLength && std::memcmp(lyraStringBytes(value), name, length) == 0;
}

}

extern "C" const void* lyraReflectTypeName(const void* type) {
    const auto* info = descriptor(type);
    return info && info->name ? lyraStringFromBytes(info->name, std::strlen(info->name)) : lyraStringFromBytes("", 0);
}

extern "C" std::int64_t lyraReflectFieldCount(const void* type) {
    const auto* info = descriptor(type);
    return info ? info->fieldCount : 0;
}

extern "C" const void* lyraReflectFieldNames(const void* type) {
    const auto* info = descriptor(type);
    auto* names = lyraListCreate();
    if (!info) return names;
    for (std::int64_t index = 0; index < info->fieldCount; ++index) {
        auto name = lyraStringFromBytes(info->fields[index].name, std::strlen(info->fields[index].name));
        auto* boxed = lyraGcAlloc(sizeof(name));
        std::memcpy(boxed, &name, sizeof(name));
        lyraListAdd(names, boxed);
    }
    return names;
}

extern "C" std::uint8_t lyraReflectFieldValue(const void* value, const void* name, void* output) {
    if (!value || !name || !output) return false;
    const auto& any = *static_cast<const LyraAnyValue*>(value);
    const auto* info = descriptor(any.type);
    if (!info) return false;
    for (std::int64_t index = 0; index < info->fieldCount; ++index) {
        const auto& field = info->fields[index];
        if (equals(name, field.name)) {
            field.get(any.data, output);
            return true;
        }
    }
    return false;
}
