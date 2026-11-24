#include "managed.h"
#include "strings.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <tomlc17.h>

namespace {
enum class Kind : std::int64_t { Missing, Table, Array, Text, Integer, Boolean, Float, Temporal, Error };
struct Node;
struct Entry { const void* key; const Node* value; };
struct Node {
    Kind kind;
    union { std::int64_t integer; double real; const void* text; Entry* entries; } data;
    std::size_t size;
};

Node* node(Kind kind) {
    auto* value = static_cast<Node*>(lyraGcAlloc(sizeof(Node)));
    *value = {kind, {}, 0};
    return value;
}

const void* text(const char* bytes) { return lyraStringFromBytes(bytes, std::strlen(bytes)); }

const Node* error(const char* message) {
    auto* value = node(Kind::Error);
    value->data.text = text(message);
    return value;
}

const Node* copy(const toml_datum_t& item, unsigned depth) {
    if (depth > 128) return error("TOML nesting exceeds 128 levels");
    switch (item.type) {
        case TOML_TABLE: case TOML_ARRAY: {
            bool table = item.type == TOML_TABLE;
            auto* value = node(table ? Kind::Table : Kind::Array);
            value->size = table ? item.u.tab.size : item.u.arr.size;
            if (value->size) value->data.entries = static_cast<Entry*>(lyraGcAlloc(value->size * sizeof(Entry)));
            for (std::size_t index = 0; index < value->size; ++index) {
                const auto* child = copy(table ? item.u.tab.value[index] : item.u.arr.elem[index], depth + 1);
                if (child->kind == Kind::Error) return child;
                auto key = table ? lyraStringFromBytes(item.u.tab.key[index], item.u.tab.len[index]) : nullptr;
                value->data.entries[index] = {key, child};
            }
            return value;
        }
        case TOML_STRING: {
            auto* value = node(Kind::Text);
            value->data.text = lyraStringFromBytes(item.u.str.ptr, item.u.str.len);
            return value;
        }
        case TOML_INT64: case TOML_BOOLEAN: {
            auto* value = node(item.type == TOML_BOOLEAN ? Kind::Boolean : Kind::Integer);
            value->data.integer = item.type == TOML_BOOLEAN ? item.u.boolean : item.u.int64;
            return value;
        }
        case TOML_FP64: {
            auto* value = node(Kind::Float);
            value->data.real = item.u.fp64;
            return value;
        }
        case TOML_DATE: case TOML_TIME: case TOML_DATETIME: case TOML_DATETIMETZ: return node(Kind::Temporal);
        default: std::abort();
    }
}

const void* floatText(double value, const void* digits) {
    if (std::isnan(value)) return text(std::signbit(value) ? "-nan" : "nan");
    if (std::isinf(value)) return text(std::signbit(value) ? "-inf" : "inf");
    const char* bytes = lyraStringBytes(digits);
    auto size = lyraStringLength(digits);
    if (std::memchr(bytes, '.', size) || std::memchr(bytes, 'e', size)) return digits;
    return lyraStringConcat(digits, text(".0"));
}
}

extern "C" {
const Node* lyraTomlParse(const void* source) {
    auto size = lyraStringLength(source);
    auto bytes = lyraStringBytes(source);
    if (size > 16 * 1024 * 1024) return error("TOML input exceeds 16 MiB");
    if (std::memchr(bytes, '\0', size)) return error("TOML input contains an unescaped NUL");
    auto* buffer = static_cast<char*>(lyraGcAllocAtomic(size + 1));
    std::memcpy(buffer, bytes, size);
    buffer[size] = '\0';
    auto document = toml_parse(buffer, static_cast<int>(size));
    if (!document.ok) return error(document.errmsg);
    auto* value = copy(document.toptab, 0);
    toml_free(document);
    return value;
}

std::int64_t lyraTomlKind(const Node* node) { return static_cast<std::int64_t>(node ? node->kind : Kind::Missing); }
std::int64_t lyraTomlSize(const Node* node) { return node ? static_cast<std::int64_t>(node->size) : 0; }
std::int64_t lyraTomlInteger(const Node* node) { return node->data.integer; }
double lyraTomlFloat(const Node* node) { return node->data.real; }
const void* lyraTomlText(const Node* node) { return node->data.text; }
const void* lyraTomlKey(const Node* node, std::int64_t index) {
    return node && node->kind == Kind::Table && index >= 0 && static_cast<std::size_t>(index) < node->size ? node->data.entries[index].key : text("");
}
const Node* lyraTomlItem(const Node* node, std::int64_t index) {
    return node && node->kind == Kind::Array && index >= 0 && static_cast<std::size_t>(index) < node->size ? node->data.entries[index].value : nullptr;
}
const Node* lyraTomlField(const Node* node, const void* key) {
    if (node && node->kind == Kind::Table) for (std::size_t index = 0; index < node->size; ++index) {
        const auto& entry = node->data.entries[index];
        if (lyraStringEqual(entry.key, key)) return entry.value;
    }
    return nullptr;
}

const void* lyraTomlQuote(const void* input) {
    auto size = lyraStringLength(input);
    if (size > (std::numeric_limits<std::size_t>::max() - 2) / 6) std::abort();
    auto bytes = reinterpret_cast<const unsigned char*>(lyraStringBytes(input));
    std::size_t length = 2;
    for (std::size_t index = 0; index < size; ++index) {
        auto byte = bytes[index];
        length += byte < 32 || byte == 127 ? 6 : (byte == '"' || byte == '\\' ? 2 : 1);
    }
    auto* buffer = static_cast<char*>(lyraGcAllocAtomic(length));
    auto* out = buffer;
    *out++ = '"';
    constexpr char hex[] = "0123456789ABCDEF";
    for (std::size_t index = 0; index < size; ++index) {
        auto byte = bytes[index];
        if (byte < 32 || byte == 127) {
            std::memcpy(out, "\\u00", 4);
            out += 4;
            *out++ = hex[byte >> 4];
            *out++ = hex[byte & 15];
        } else {
            if (byte == '"' || byte == '\\') *out++ = '\\';
            *out++ = static_cast<char>(byte);
        }
    }
    *out = '"';
    return lyraStringFromBytes(buffer, length);
}

const void* lyraTomlFloat32Text(float value) { return floatText(value, lyraFloat32Text(value)); }
const void* lyraTomlFloat64Text(double value) { return floatText(value, lyraFloat64Text(value)); }
std::uint8_t lyraTomlIsFinite(double value) { return std::isfinite(value); }
}
