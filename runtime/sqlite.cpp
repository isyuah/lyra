#include "sqlite.h"
#include "managed.h"
#include "strings.h"

#include <atomic>
#include <ctype.h>
#include <limits.h>
#include <new>
#include <sqlite3.h>
#include <string.h>

struct LyraSqliteConnection {
    std::atomic_flag busy{};
    sqlite3 *raw;
};

enum class Cursor { Before, Row, Done, Failed };

struct LyraSqliteStatement {
    LyraSqliteConnection *owner;
    sqlite3_stmt *raw;
    Cursor cursor;
    LyraSqliteError failure;
};

namespace {

struct Lease {
    explicit Lease(LyraSqliteConnection *connection) : connection(connection), held(!connection->busy.test_and_set(std::memory_order_acquire)) {}
    ~Lease() { if (held) connection->busy.clear(std::memory_order_release); }
    LyraSqliteConnection *connection;
    bool held;
};

LyraSqliteStatus problem(LyraSqliteError *error, const char *operation, LyraSqliteStatus kind, int code, const char *message) {
    *error = {operation, lyraStringFromBytes(message, strlen(message)), code};
    return kind;
}

LyraSqliteStatus database(LyraSqliteError *error, const char *operation, sqlite3 *db, int code) {
    return problem(error, operation, LYRA_SQLITE_DB_ERROR, code, db ? sqlite3_errmsg(db) : sqlite3_errstr(code));
}

LyraSqliteStatus unavailable(LyraSqliteError *error, const char *operation, bool held) {
    return held ? problem(error, operation, LYRA_SQLITE_CLOSED, SQLITE_MISUSE, "handle is closed")
                : problem(error, operation, LYRA_SQLITE_BUSY, SQLITE_BUSY, "connection is in use");
}

LyraSqliteStatus access(const Lease &lease, LyraSqliteStatement *statement, LyraSqliteError *error, const char *operation) {
    if (!lease.held || !statement->raw) return unavailable(error, operation, lease.held);
    return LYRA_SQLITE_OK;
}

LyraSqliteStatus columnType(const Lease &lease, LyraSqliteStatement *statement, int column, int type, LyraSqliteError *error, const char *operation) {
    auto status = access(lease, statement, error, operation);
    if (status != LYRA_SQLITE_OK) return status;
    if (statement->cursor != Cursor::Row || column < 0 || column >= sqlite3_column_count(statement->raw))
        return problem(error, operation, LYRA_SQLITE_INVALID, SQLITE_RANGE, "column or cursor position is invalid");
    int actual = sqlite3_column_type(statement->raw, column);
    if (actual == SQLITE_NULL) return LYRA_SQLITE_NULL;
    if (actual != type) return problem(error, operation, LYRA_SQLITE_INVALID, SQLITE_MISMATCH, "column has a different storage type");
    return LYRA_SQLITE_OK;
}

}

LyraSqliteStatus lyraSqliteOpen(const void *path, int readOnly, LyraSqliteConnection **output, LyraSqliteError *error) {
    *output = nullptr;
    size_t count = lyraStringLength(path);
    const char *bytes = lyraStringBytes(path);
    if (count > INT_MAX || memchr(bytes, 0, count)) return problem(error, "open", LYRA_SQLITE_INVALID, SQLITE_MISUSE, "invalid database path");
    auto *terminated = static_cast<char *>(lyraGcAllocAtomic(count + 1));
    memcpy(terminated, bytes, count);
    terminated[count] = 0;
    sqlite3 *raw = nullptr;
    auto flags = readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    int code = sqlite3_open_v2(terminated, &raw, flags | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (code != SQLITE_OK) {
        auto status = database(error, "open", raw, code);
        sqlite3_close(raw);
        return status;
    }
    auto *connection = ::new (lyraGcAlloc(sizeof(LyraSqliteConnection))) LyraSqliteConnection{};
    connection->raw = raw;
    *output = connection;
    return LYRA_SQLITE_OK;
}

LyraSqliteStatus lyraSqlitePrepare(LyraSqliteConnection *connection, const void *sql, LyraSqliteStatement **output, LyraSqliteError *error) {
    *output = nullptr;
    Lease lease(connection);
    if (!lease.held || !connection->raw) return unavailable(error, "prepare", lease.held);
    size_t length = lyraStringLength(sql);
    const char *bytes = lyraStringBytes(sql);
    if (length > INT_MAX || memchr(bytes, 0, length)) return problem(error, "prepare", LYRA_SQLITE_INVALID, SQLITE_MISUSE, "SQL contains NUL or exceeds the length limit");
    sqlite3_stmt *raw = nullptr;
    const char *tail = nullptr;
    int code = sqlite3_prepare_v2(connection->raw, bytes, static_cast<int>(length), &raw, &tail);
    if (code != SQLITE_OK) return database(error, "prepare", connection->raw, code);
    while (tail != bytes + length && isspace(static_cast<unsigned char>(*tail))) ++tail;
    if (!raw || tail != bytes + length) {
        sqlite3_finalize(raw);
        return problem(error, "prepare", LYRA_SQLITE_INVALID, SQLITE_MISUSE, "expected exactly one SQL statement");
    }
    auto *statement = ::new (lyraGcAlloc(sizeof(LyraSqliteStatement))) LyraSqliteStatement{};
    statement->owner = connection;
    statement->raw = raw;
    *output = statement;
    return LYRA_SQLITE_OK;
}

LyraSqliteStatus lyraSqliteBindInt(LyraSqliteStatement *statement, int position, int64_t value, LyraSqliteError *error) {
    Lease lease(statement->owner);
    auto status = access(lease, statement, error, "bindInt");
    if (status != LYRA_SQLITE_OK) return status;
    auto code = sqlite3_bind_int64(statement->raw, position, value);
    return code == SQLITE_OK ? LYRA_SQLITE_OK : database(error, "bindInt", statement->owner->raw, code);
}

LyraSqliteStatus lyraSqliteBindText(LyraSqliteStatement *statement, int position, const void *value, LyraSqliteError *error) {
    Lease lease(statement->owner);
    auto status = access(lease, statement, error, "bindText");
    if (status != LYRA_SQLITE_OK) return status;
    auto code = sqlite3_bind_text64(statement->raw, position, lyraStringBytes(value), lyraStringLength(value), SQLITE_TRANSIENT, SQLITE_UTF8);
    return code == SQLITE_OK ? LYRA_SQLITE_OK : database(error, "bindText", statement->owner->raw, code);
}

LyraSqliteStatus lyraSqliteNext(LyraSqliteStatement *statement, LyraSqliteError *error) {
    Lease lease(statement->owner);
    auto status = access(lease, statement, error, "next");
    if (status != LYRA_SQLITE_OK) return status;
    if (statement->cursor == Cursor::Done) return LYRA_SQLITE_DONE;
    if (statement->cursor == Cursor::Failed) {
        *error = statement->failure;
        return LYRA_SQLITE_DB_ERROR;
    }
    auto code = sqlite3_step(statement->raw);
    if (code == SQLITE_ROW || code == SQLITE_DONE) {
        statement->cursor = code == SQLITE_ROW ? Cursor::Row : Cursor::Done;
        return code == SQLITE_ROW ? LYRA_SQLITE_ROW : LYRA_SQLITE_DONE;
    }
    statement->cursor = Cursor::Failed;
    auto failed = database(error, "next", statement->owner->raw, code);
    statement->failure = *error;
    return failed;
}

LyraSqliteStatus lyraSqliteReadInt(LyraSqliteStatement *statement, int column, int64_t *output, LyraSqliteError *error) {
    Lease lease(statement->owner);
    auto status = columnType(lease, statement, column, SQLITE_INTEGER, error, "int");
    if (status == LYRA_SQLITE_OK) *output = sqlite3_column_int64(statement->raw, column);
    return status;
}

LyraSqliteStatus lyraSqliteReadText(LyraSqliteStatement *statement, int column, const void **output, LyraSqliteError *error) {
    *output = nullptr;
    Lease lease(statement->owner);
    auto status = columnType(lease, statement, column, SQLITE_TEXT, error, "text");
    if (status != LYRA_SQLITE_OK) return status;
    auto *bytes = sqlite3_column_text(statement->raw, column);
    if (!bytes) return database(error, "text", statement->owner->raw, SQLITE_NOMEM);
    int count = sqlite3_column_bytes(statement->raw, column);
    *output = lyraStringFromBytes(reinterpret_cast<const char *>(bytes), static_cast<size_t>(count));
    return LYRA_SQLITE_OK;
}

LyraSqliteStatus lyraSqliteFinalize(LyraSqliteStatement *statement, LyraSqliteError *error) {
    Lease lease(statement->owner);
    if (!lease.held) return unavailable(error, "finalize", false);
    if (!statement->raw) return LYRA_SQLITE_OK;
    int code = sqlite3_finalize(statement->raw);
    statement->raw = nullptr;
    return code == SQLITE_OK ? LYRA_SQLITE_OK : database(error, "finalize", statement->owner->raw, code);
}

LyraSqliteStatus lyraSqliteClose(LyraSqliteConnection *connection, LyraSqliteError *error) {
    Lease lease(connection);
    if (!lease.held) return unavailable(error, "close", false);
    if (!connection->raw) return LYRA_SQLITE_OK;
    int code = sqlite3_close(connection->raw);
    if (code == SQLITE_BUSY) return problem(error, "close", LYRA_SQLITE_BUSY, code, sqlite3_errmsg(connection->raw));
    if (code != SQLITE_OK) return database(error, "close", connection->raw, code);
    connection->raw = nullptr;
    return LYRA_SQLITE_OK;
}
