#include "managed.h"
#include "sqlite.h"
#include "strings.h"

#include <climits>
#include <cstdint>
#include <cstring>
#include <new>
#include <sqlite3.h>

namespace {

struct Reply {
    LyraSqliteStatus status{};
    LyraSqliteError error{};
    LyraSqliteConnection* connection{};
    LyraSqliteStatement* statement{};
    int64_t number{};
    const void* text{};
};

Reply* reply() { return ::new (lyraGcAlloc(sizeof(Reply))) Reply{}; }

bool usable(Reply* output, const Reply* previous, bool available, const char* operation) {
    if (available) return true;
    output->status = previous->status;
    output->error = previous->error;
    if (output->status == LYRA_SQLITE_OK) {
        output->status = LYRA_SQLITE_INVALID;
        output->error = {operation, lyraStringFromBytes("wrong handle kind", 17), SQLITE_MISUSE};
    }
    return false;
}

bool index(Reply* output, int64_t value, bool parameter, const char* operation) {
    if (value >= (parameter ? 1 : 0) && value <= INT_MAX) return true;
    output->status = LYRA_SQLITE_INVALID;
    output->error = {operation, lyraStringFromBytes("index out of range", 18), SQLITE_RANGE};
    return false;
}

}

extern "C" Reply* lyraDbOpen(const void* path, uint8_t readOnly) {
    auto out = reply();
    out->status = lyraSqliteOpen(path, readOnly, &out->connection, &out->error);
    return out;
}

extern "C" Reply* lyraDbPrepare(const Reply* db, const void* sql) {
    auto out = reply();
    if (usable(out, db, db->connection, "prepare")) out->status = lyraSqlitePrepare(db->connection, sql, &out->statement, &out->error);
    return out;
}

extern "C" Reply* lyraDbClose(const Reply* db) {
    auto out = reply();
    if (usable(out, db, db->connection, "close")) out->status = lyraSqliteClose(db->connection, &out->error);
    return out;
}

extern "C" Reply* lyraDbFinalize(const Reply* cursor) {
    auto out = reply();
    if (usable(out, cursor, cursor->statement, "finalize")) out->status = lyraSqliteFinalize(cursor->statement, &out->error);
    return out;
}

extern "C" Reply* lyraDbNext(const Reply* cursor) {
    auto out = reply();
    if (usable(out, cursor, cursor->statement, "step")) out->status = lyraSqliteNext(cursor->statement, &out->error);
    return out;
}

extern "C" Reply* lyraDbBindInt(const Reply* cursor, int64_t position, int64_t number) {
    auto out = reply();
    if (usable(out, cursor, cursor->statement, "bindInt") && index(out, position, true, "bindInt")) out->status = lyraSqliteBindInt(cursor->statement, static_cast<int>(position), number, &out->error);
    return out;
}

extern "C" Reply* lyraDbBindText(const Reply* cursor, int64_t position, const void* text) {
    auto out = reply();
    if (usable(out, cursor, cursor->statement, "bindText") && index(out, position, true, "bindText")) out->status = lyraSqliteBindText(cursor->statement, static_cast<int>(position), text, &out->error);
    return out;
}

extern "C" Reply* lyraDbIntAt(const Reply* cursor, int64_t column) {
    auto out = reply();
    if (usable(out, cursor, cursor->statement, "readInt") && index(out, column, false, "readInt")) out->status = lyraSqliteReadInt(cursor->statement, static_cast<int>(column), &out->number, &out->error);
    return out;
}

extern "C" Reply* lyraDbTextAt(const Reply* cursor, int64_t column) {
    auto out = reply();
    if (usable(out, cursor, cursor->statement, "readText") && index(out, column, false, "readText")) out->status = lyraSqliteReadText(cursor->statement, static_cast<int>(column), &out->text, &out->error);
    if (out->status == LYRA_SQLITE_OK && !lyraUtf8Valid(lyraStringBytes(out->text), lyraStringLength(out->text))) {
        constexpr char message[] = "text column is not valid UTF-8";
        out->text = nullptr;
        out->status = LYRA_SQLITE_INVALID;
        out->error = {"readText", lyraStringFromBytes(message, sizeof(message) - 1), SQLITE_MISMATCH};
    }
    return out;
}

extern "C" int64_t lyraDbStatus(const Reply* result) { return result->status; }
extern "C" int64_t lyraDbCode(const Reply* result) { return result->error.code; }
extern "C" int64_t lyraDbNumber(const Reply* result) { return result->number; }
extern "C" const void* lyraDbText(const Reply* result) { return result->text ? result->text : lyraStringFromBytes(nullptr, 0); }
extern "C" const void* lyraDbMessage(const Reply* result) { return result->error.message ? result->error.message : lyraStringFromBytes(nullptr, 0); }
extern "C" const void* lyraDbOperation(const Reply* result) {
    auto name = result->error.operation;
    return lyraStringFromBytes(name, name ? std::strlen(name) : 0);
}
