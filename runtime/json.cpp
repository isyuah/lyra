#include "managed.h"
#include "strings.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <jansson.h>
#include <limits>

namespace {
enum class Kind : std::int64_t { Missing, Null, Object, Array, Text, Integer, Boolean, Real, Invalid };
struct Node;
struct Entry { const void* key; Node* value; };
struct Node {
    Kind kind;
    union { std::int64_t number; double real; };
    const void* text;
    std::size_t size;
    Entry* entries;
};

Node* node(Kind kind) {
    auto* value = static_cast<Node*>(lyraGcAlloc(sizeof(Node)));
    *value = {kind, {0}, nullptr, 0, nullptr};
    return value;
}

const void* str(const char* value) { return lyraStringFromBytes(value, std::strlen(value)); }

const void* scalarText(json_t* value, std::size_t flags = 0) {
    if (!value) std::abort();
    auto* bytes = json_dumps(value, JSON_ENCODE_ANY | JSON_COMPACT | flags);
    json_decref(value);
    if (!bytes) std::abort();
    auto* result = str(bytes); std::free(bytes); return result;
}

Node* convert(json_t* json, unsigned depth) {
    if (depth > 128) return nullptr;
    auto* value = node(Kind::Null);
    if (json_is_object(json) || json_is_array(json)) {
        bool object = json_is_object(json);
        value->kind = object ? Kind::Object : Kind::Array;
        value->size = object ? json_object_size(json) : json_array_size(json);
        if (value->size) value->entries = static_cast<Entry*>(lyraGcAlloc(value->size * sizeof(Entry)));
        auto* field = object ? json_object_iter(json) : nullptr;
        for (std::size_t index = 0; index < value->size; ++index) {
            auto* child = convert(object ? json_object_iter_value(field) : json_array_get(json, index), depth + 1);
            if (!child) return nullptr;
            value->entries[index] = {object ? str(json_object_iter_key(field)) : nullptr, child};
            if (object) field = json_object_iter_next(json, field);
        }
    } else if (json_is_string(json)) {
        value->kind = Kind::Text; value->text = lyraStringFromBytes(json_string_value(json), json_string_length(json));
    } else if (json_is_integer(json)) { value->kind = Kind::Integer; value->number = json_integer_value(json); }
    else if (json_is_boolean(json)) { value->kind = Kind::Boolean; value->number = json_is_true(json); }
    else if (json_is_real(json)) { value->kind = Kind::Real; value->real = json_real_value(json); }
    return value;
}
}

extern "C" {
const Node* lyraJsonParse(const void* input) {
    json_error_t error;
    auto* json = json_loadb(lyraStringBytes(input), lyraStringLength(input), JSON_DECODE_ANY | JSON_REJECT_DUPLICATES | JSON_ALLOW_NUL, &error);
    if (json) {
        auto* value = convert(json, 0);
        json_decref(json);
        if (value) return value;
    }
    auto* invalid = node(Kind::Invalid);
    char message[320];
    if (json) invalid->text = str("JSON nesting exceeds 128 levels");
    else { std::snprintf(message, sizeof(message), "JSON %d:%d: %s", error.line, error.column, error.text); invalid->text = str(message); }
    return invalid;
}

std::int64_t lyraJsonKind(const Node* value) { return static_cast<std::int64_t>(value ? value->kind : Kind::Missing); }
std::int64_t lyraJsonSize(const Node* value) { return value ? static_cast<std::int64_t>(value->size) : 0; }
std::int64_t lyraJsonNumber(const Node* value) { return value->number; }
double lyraJsonReal(const Node* value) { return value->real; }
std::uint8_t lyraJsonIsFinite(double value) { return std::isfinite(value); }
const void* lyraJsonFloat32Text(float value) { return scalarText(json_real(value), JSON_REAL_PRECISION(std::numeric_limits<float>::max_digits10)); }
const void* lyraJsonFloat64Text(double value) { return scalarText(json_real(value), JSON_REAL_PRECISION(std::numeric_limits<double>::max_digits10)); }
const void* lyraJsonText(const Node* value) { return value->text; }
const void* lyraJsonKey(const Node* value, std::int64_t index) {
    return value && value->kind == Kind::Object && index >= 0 && static_cast<std::size_t>(index) < value->size ? value->entries[index].key : str("");
}
const Node* lyraJsonIndex(const Node* value, std::int64_t index) {
    return value && value->kind == Kind::Array && index >= 0 && static_cast<std::size_t>(index) < value->size ? value->entries[index].value : nullptr;
}
const Node* lyraJsonFieldAt(const Node* value, std::int64_t index) {
    return value && value->kind == Kind::Object && index >= 0 && static_cast<std::size_t>(index) < value->size ? value->entries[index].value : nullptr;
}
const Node* lyraJsonField(const Node* value, const void* key) {
    if (value && value->kind == Kind::Object) for (std::size_t index = 0; index < value->size; ++index) {
        if (lyraStringEqual(key, value->entries[index].key)) return value->entries[index].value;
    }
    return nullptr;
}

std::uint8_t lyraJsonSimpleKey(const void* key) {
    const auto* bytes = lyraStringBytes(key);
    const auto size = lyraStringLength(key);
    for (std::size_t index = 0; index < size; ++index) {
        auto byte = bytes[index];
        if (!(byte == '_' || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (index && byte >= '0' && byte <= '9'))) return false;
    }
    return size != 0;
}

const void* lyraJsonQuote(const void* text) {
    return scalarText(json_stringn_nocheck(lyraStringBytes(text), lyraStringLength(text)));
}
const void* lyraJsonIntText(std::int64_t value) {
    char buffer[32];
    auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return lyraStringFromBytes(buffer, converted.ptr - buffer);
}
}
