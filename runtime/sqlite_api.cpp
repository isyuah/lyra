#include "managed.h"
#include "sqlite.h"
#include "strings.h"

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
    LyraSqliteTransaction* transaction{};
    int64_t number{};
    double real{};
    const void* text{};
    const Reply* cleanup{};
};

Reply* reply() { return ::new (lyraGcAlloc(sizeof(Reply))) Reply{}; }

}

extern "C" Reply* lyraDbOpen(const void* path, uint8_t readOnly) {
    auto out = reply();
    out->status = lyraSqliteOpen(path, readOnly, &out->connection, &out->error);
    return out;
}

extern "C" Reply* lyraDbPrepare(const Reply* db, const void* sql) {
    auto out = reply();
    out->status = lyraSqlitePrepare(db->connection, sql, &out->statement, &out->error);
    return out;
}

extern "C" Reply* lyraDbExec(const Reply* db, const void* sql) {
    auto out = reply();
    out->status = lyraSqliteExec(db->connection, sql, &out->error);
    return out;
}

extern "C" Reply* lyraDbLastInsertRowid(const Reply* db) {
    auto out = reply();
    out->status = lyraSqliteLastInsertRowid(db->connection, &out->number, &out->error);
    return out;
}

extern "C" Reply* lyraDbBegin(const Reply* db) {
    auto out = reply();
    out->status = lyraSqliteBegin(db->connection, &out->transaction, &out->error);
    return out;
}

extern "C" Reply* lyraDbFinish(const Reply* scope, uint8_t commit) {
    auto out = reply();
    LyraSqliteIssue cleanup;
    out->status = lyraSqliteFinish(scope->transaction, commit, &out->error, &cleanup);
    if (cleanup.status != LYRA_SQLITE_OK) {
        auto other = reply();
        other->status = cleanup.status;
        other->error = cleanup.error;
        out->cleanup = other;
    }
    return out;
}

extern "C" Reply* lyraDbTransactionPrepare(const Reply* scope, const void* sql) {
    auto out = reply();
    out->status = lyraSqliteTransactionPrepare(scope->transaction, sql, &out->statement, &out->error);
    return out;
}

extern "C" Reply* lyraDbTransactionExec(const Reply* scope, const void* sql) {
    auto out = reply();
    out->status = lyraSqliteTransactionExec(scope->transaction, sql, &out->error);
    return out;
}

extern "C" Reply* lyraDbTransactionLastInsertRowid(const Reply* scope) {
    auto out = reply();
    out->status = lyraSqliteTransactionLastInsertRowid(scope->transaction, &out->number, &out->error);
    return out;
}

extern "C" Reply* lyraDbClose(const Reply* db) {
    auto out = reply();
    out->status = lyraSqliteClose(db->connection, &out->error);
    return out;
}

extern "C" Reply* lyraDbFinalize(const Reply* cursor) {
    auto out = reply();
    out->status = lyraSqliteFinalize(cursor->statement, &out->error);
    return out;
}

extern "C" Reply* lyraDbNext(const Reply* cursor) {
    auto out = reply();
    out->status = lyraSqliteNext(cursor->statement, &out->error);
    return out;
}

extern "C" Reply* lyraDbBindInt(const Reply* cursor, int64_t position, int64_t number) {
    auto out = reply();
    out->status = lyraSqliteBindInt(cursor->statement, position, number, &out->error);
    return out;
}

extern "C" Reply* lyraDbBindText(const Reply* cursor, int64_t position, const void* text) {
    auto out = reply();
    out->status = lyraSqliteBindText(cursor->statement, position, text, &out->error);
    return out;
}

extern "C" Reply* lyraDbBindFloat(const Reply* cursor, int64_t position, double value) {
    auto out = reply();
    out->status = lyraSqliteBindFloat(cursor->statement, position, value, &out->error);
    return out;
}

extern "C" Reply* lyraDbBindBlob(const Reply* cursor, int64_t position, const void* value) {
    auto out = reply();
    out->status = lyraSqliteBindBlob(cursor->statement, position, value, &out->error);
    return out;
}

extern "C" Reply* lyraDbBindNull(const Reply* cursor, int64_t position) {
    auto out = reply();
    out->status = lyraSqliteBindNull(cursor->statement, position, &out->error);
    return out;
}

extern "C" Reply* lyraDbIntAt(const Reply* cursor, int64_t column) {
    auto out = reply();
    out->status = lyraSqliteReadInt(cursor->statement, column, &out->number, &out->error);
    return out;
}

extern "C" Reply* lyraDbTextAt(const Reply* cursor, int64_t column) {
    auto out = reply();
    out->status = lyraSqliteReadText(cursor->statement, column, &out->text, &out->error);
    if (out->status == LYRA_SQLITE_OK && !lyraUtf8Valid(lyraStringBytes(out->text), lyraStringLength(out->text))) {
        constexpr char message[] = "text column is not valid UTF-8";
        out->text = nullptr;
        out->status = LYRA_SQLITE_INVALID;
        out->error = {"readText", lyraStringFromBytes(message, sizeof(message) - 1), SQLITE_MISMATCH};
    }
    return out;
}

extern "C" Reply* lyraDbFloatAt(const Reply* cursor, int64_t column) {
    auto out = reply();
    out->status = lyraSqliteReadFloat(cursor->statement, column, &out->real, &out->error);
    return out;
}

extern "C" Reply* lyraDbBlobAt(const Reply* cursor, int64_t column) {
    auto out = reply();
    out->status = lyraSqliteReadBlob(cursor->statement, column, &out->text, &out->error);
    return out;
}

extern "C" int64_t lyraDbStatus(const Reply* result) { return result->status; }
extern "C" int64_t lyraDbCode(const Reply* result) { return result->error.code; }
extern "C" int64_t lyraDbNumber(const Reply* result) { return result->number; }
extern "C" double lyraDbFloat(const Reply* result) { return result->real; }
extern "C" const void* lyraDbText(const Reply* result) { return result->text; }
extern "C" const void* lyraDbBlob(const Reply* result) { return result->text; }
extern "C" const void* lyraDbMessage(const Reply* result) { return result->error.message; }
extern "C" uint8_t lyraDbHasCleanup(const Reply* result) { return result->cleanup != nullptr; }
extern "C" const Reply* lyraDbCleanup(const Reply* result) { return result->cleanup; }
extern "C" const void* lyraDbOperation(const Reply* result) {
    auto name = result->error.operation;
    return lyraStringFromBytes(name, std::strlen(name));
}
