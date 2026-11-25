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
constexpr unsigned maxDepth = 128;
constexpr char nestingError[] = "JSON nesting exceeds 128 levels";

enum class Kind : std::int64_t { Missing, Null, Object, Array, Text, Integer, Boolean, Real, Invalid };
struct Node;
struct Entry { const void* key; const Node* value; };
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

Node* textNode(Kind kind, const void* text) {
    auto* value = node(kind);
    value->text = text;
    return value;
}

Node* integerNode(Kind kind, std::int64_t number) {
    auto* value = node(kind);
    value->number = number;
    return value;
}

const void* dumpText(json_t* value, std::size_t flags) {
    if (!value) std::abort();
    auto* bytes = json_dumps(value, JSON_ENCODE_ANY | flags);
    json_decref(value);
    if (!bytes) std::abort();
    auto* result = str(bytes); std::free(bytes); return result;
}

Node* convert(json_t* json, unsigned depth) {
    if (depth > maxDepth) return nullptr;
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

json_t* encodeTree(const Node* value, unsigned depth) {
    if (depth > maxDepth) return nullptr;
    json_t* json = nullptr;
    switch (value->kind) {
    case Kind::Null: json = json_null(); break;
    case Kind::Text: json = json_stringn_nocheck(lyraStringBytes(value->text), lyraStringLength(value->text)); break;
    case Kind::Integer: json = json_integer(value->number); break;
    case Kind::Boolean: json = json_boolean(value->number); break;
    case Kind::Real: json = json_real(value->real); break;
    case Kind::Object:
    case Kind::Array: {
        const bool object = value->kind == Kind::Object;
        json = object ? json_object() : json_array();
        if (!json) std::abort();
        for (std::size_t index = 0; index < value->size; ++index) {
            const auto& entry = value->entries[index];
            auto* child = encodeTree(entry.value, depth + 1);
            if (!child) { json_decref(json); return nullptr; }
            int status;
            if (object) {
                auto size = lyraStringLength(entry.key);
                auto* key = static_cast<char*>(std::malloc(size + 1));
                if (!key) std::abort();
                std::memcpy(key, lyraStringBytes(entry.key), size);
                key[size] = '\0';
                status = json_object_set_new_nocheck(json, key, child);
                std::free(key);
            } else {
                status = json_array_append_new(json, child);
            }
            if (status != 0) std::abort();
        }
        break;
    }
    case Kind::Missing:
    case Kind::Invalid: std::abort();
    }
    if (!json) std::abort();
    return json;
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
        return textNode(Kind::Invalid, str(nestingError));
    }
    char message[320];
    std::snprintf(message, sizeof(message), "JSON %d:%d: %s", error.line, error.column, error.text);
    return textNode(Kind::Invalid, str(message));
}

const Node* lyraJsonNull() { return node(Kind::Null); }
const Node* lyraJsonBool(std::uint8_t value) { return integerNode(Kind::Boolean, value); }
const Node* lyraJsonInt(std::int64_t value) { return integerNode(Kind::Integer, value); }
const Node* lyraJsonFloat(double real) {
    auto* value = node(Kind::Real);
    value->real = real;
    return value;
}
const Node* lyraJsonString(const void* text) { return textNode(Kind::Text, text); }

Node* lyraJsonContainer(std::int64_t size, std::uint8_t object) {
    auto* value = node(object ? Kind::Object : Kind::Array);
    value->size = static_cast<std::size_t>(size);
    if (value->size) value->entries = static_cast<Entry*>(lyraGcAlloc(value->size * sizeof(Entry)));
    return value;
}

void lyraJsonSetEntry(Node* container, std::int64_t index, const void* key, const Node* value) {
    container->entries[index] = {key, value};
}

const Node* lyraJsonEncode(const Node* value, std::uint8_t pretty) {
    auto* json = encodeTree(value, 0);
    if (!json) return textNode(Kind::Invalid, str(nestingError));
    auto flags = (pretty ? JSON_INDENT(2) : JSON_COMPACT) | JSON_REAL_PRECISION(std::numeric_limits<double>::max_digits10);
    return textNode(Kind::Text, dumpText(json, flags));
}

std::int64_t lyraJsonKind(const Node* value) { return static_cast<std::int64_t>(value ? value->kind : Kind::Missing); }
std::int64_t lyraJsonSize(const Node* value) { return value ? static_cast<std::int64_t>(value->size) : 0; }
std::int64_t lyraJsonNumber(const Node* value) { return value->number; }
double lyraJsonReal(const Node* value) { return value->real; }
std::uint8_t lyraJsonIsFinite(double value) { return std::isfinite(value); }
const void* lyraJsonFloat32Text(float value) { return dumpText(json_real(value), JSON_COMPACT | JSON_REAL_PRECISION(std::numeric_limits<float>::max_digits10)); }
const void* lyraJsonFloat64Text(double value) { return dumpText(json_real(value), JSON_COMPACT | JSON_REAL_PRECISION(std::numeric_limits<double>::max_digits10)); }
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
    return dumpText(json_stringn_nocheck(lyraStringBytes(text), lyraStringLength(text)), JSON_COMPACT);
}
const void* lyraJsonIntText(std::int64_t value) {
    char buffer[32];
    auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return lyraStringFromBytes(buffer, converted.ptr - buffer);
}
}
