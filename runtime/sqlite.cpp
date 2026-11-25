#include "sqlite.h"
#include "managed.h"
#include "strings.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <ctype.h>
#include <limits.h>
#include <new>
#include <sqlite3.h>
#include <string.h>

struct LyraSqliteConnection {
    std::atomic_flag busy{};
    sqlite3 *raw;
};

struct LyraSqliteTransaction {
    LyraSqliteConnection *owner;
    const void *thread;
    std::atomic<bool> active{true};
    LyraSqliteStatement *statements{};
};

enum class Cursor { Before, Row, Done, Failed };

struct LyraSqliteStatement {
    LyraSqliteConnection *owner;
    LyraSqliteTransaction *transaction;
    LyraSqliteStatement *previous;
    LyraSqliteStatement *next;
    sqlite3_stmt *raw;
    Cursor cursor;
    LyraSqliteIssue failure;
};

namespace {

thread_local char currentThread;

struct Lease {
    explicit Lease(LyraSqliteConnection *connection, LyraSqliteTransaction *transaction = nullptr)
        : connection(connection), transaction(transaction) {
        if (transaction) {
            if (!transaction->active.load(std::memory_order_acquire)) status = LYRA_SQLITE_CLOSED;
            else if (transaction->thread != &currentThread) status = LYRA_SQLITE_BUSY;
        } else if (connection->busy.test_and_set(std::memory_order_acquire)) {
            status = LYRA_SQLITE_BUSY;
        } else {
            release = true;
        }
    }
    explicit Lease(LyraSqliteStatement *statement) : Lease(statement->owner, statement->transaction) {}
    ~Lease() { if (release) connection->busy.clear(std::memory_order_release); }
    LyraSqliteConnection *connection;
    LyraSqliteTransaction *transaction;
    LyraSqliteStatus status = LYRA_SQLITE_OK;
    bool release = false;
};

LyraSqliteStatus problem(LyraSqliteError *error, const char *operation, LyraSqliteStatus kind, int code, const char *message) {
    *error = {operation, lyraStringFromBytes(message, strlen(message)), code};
    return kind;
}

LyraSqliteStatus database(LyraSqliteError *error, const char *operation, sqlite3 *db, int code) {
    const int primary = code & 0xff;
    const auto status = primary == SQLITE_BUSY || primary == SQLITE_LOCKED ? LYRA_SQLITE_BUSY : LYRA_SQLITE_DB_ERROR;
    return problem(error, operation, status, code, db ? sqlite3_errmsg(db) : sqlite3_errstr(code));
}

LyraSqliteStatus unavailable(LyraSqliteError *error, const char *operation, LyraSqliteStatus status) {
    return status == LYRA_SQLITE_CLOSED ? problem(error, operation, status, SQLITE_MISUSE, "handle is closed")
                                       : problem(error, operation, status, SQLITE_BUSY, "connection is in use");
}

LyraSqliteStatus access(const Lease &lease, LyraSqliteError *error, const char *operation) {
    if (lease.status != LYRA_SQLITE_OK) return unavailable(error, operation, lease.status);
    if (!lease.connection->raw) return unavailable(error, operation, LYRA_SQLITE_CLOSED);
    if (lease.transaction && sqlite3_get_autocommit(lease.connection->raw))
        return problem(error, operation, LYRA_SQLITE_CLOSED, SQLITE_ABORT, "SQLite has already rolled back this transaction");
    return LYRA_SQLITE_OK;
}

LyraSqliteStatus access(const Lease &lease, LyraSqliteStatement *statement, LyraSqliteError *error, const char *operation) {
    auto status = access(lease, error, operation);
    if (status == LYRA_SQLITE_OK && !statement->raw) return unavailable(error, operation, LYRA_SQLITE_CLOSED);
    return status;
}

LyraSqliteStatus bindPosition(const Lease &lease, LyraSqliteStatement *statement, int64_t position, LyraSqliteError *error, const char *operation) {
    auto status = access(lease, statement, error, operation);
    if (status != LYRA_SQLITE_OK) return status;
    if (position < 1 || position > sqlite3_bind_parameter_count(statement->raw))
        return problem(error, operation, LYRA_SQLITE_INVALID, SQLITE_RANGE, "parameter index out of range");
    return LYRA_SQLITE_OK;
}

LyraSqliteStatus columnType(const Lease &lease, LyraSqliteStatement *statement, int64_t column, int type, LyraSqliteError *error, const char *operation) {
    auto status = access(lease, statement, error, operation);
    if (status != LYRA_SQLITE_OK) return status;
    if (statement->cursor != Cursor::Row || column < 0 || column >= sqlite3_column_count(statement->raw))
        return problem(error, operation, LYRA_SQLITE_INVALID, SQLITE_RANGE, "column or cursor position is invalid");
    int actual = sqlite3_column_type(statement->raw, static_cast<int>(column));
    if (actual == SQLITE_NULL) return LYRA_SQLITE_NULL;
    if (actual != type) return problem(error, operation, LYRA_SQLITE_INVALID, SQLITE_MISMATCH, "column has a different storage type");
    return LYRA_SQLITE_OK;
}

LyraSqliteStatus prepareRaw(const Lease &lease, const void *sql, sqlite3_stmt **output, LyraSqliteError *error, const char *operation) {
    auto status = access(lease, error, operation);
    if (status != LYRA_SQLITE_OK) return status;
    size_t length = lyraStringLength(sql);
    const char *bytes = lyraStringBytes(sql);
    if (length > INT_MAX || memchr(bytes, 0, length)) return problem(error, operation, LYRA_SQLITE_INVALID, SQLITE_MISUSE, "SQL contains NUL or exceeds the length limit");
    sqlite3_stmt *raw = nullptr;
    const char *tail = nullptr;
    int code = sqlite3_prepare_v2(lease.connection->raw, bytes, static_cast<int>(length), &raw, &tail);
    if (code == SQLITE_AUTH && lease.transaction)
        return problem(error, operation, LYRA_SQLITE_INVALID, code, "transaction and savepoint SQL is not allowed inside transaction()");
    if (code != SQLITE_OK) return database(error, operation, lease.connection->raw, code);
    while (tail != bytes + length && isspace(static_cast<unsigned char>(*tail))) ++tail;
    if (!raw || tail != bytes + length) {
        sqlite3_finalize(raw);
        return problem(error, operation, LYRA_SQLITE_INVALID, SQLITE_MISUSE, "expected exactly one SQL statement");
    }
    *output = raw;
    return LYRA_SQLITE_OK;
}

LyraSqliteStatus prepare(const Lease &lease, const void *sql, LyraSqliteStatement **output, LyraSqliteError *error) {
    *output = nullptr;
    sqlite3_stmt *raw;
    auto status = prepareRaw(lease, sql, &raw, error, "prepare");
    if (status != LYRA_SQLITE_OK) return status;
    auto *statement = ::new (lyraGcAlloc(sizeof(LyraSqliteStatement))) LyraSqliteStatement{};
    statement->owner = lease.connection;
    statement->raw = raw;
    statement->transaction = lease.transaction;
    if (auto *transaction = lease.transaction) {
        statement->next = transaction->statements;
        if (statement->next) statement->next->previous = statement;
        transaction->statements = statement;
    }
    *output = statement;
    return LYRA_SQLITE_OK;
}

LyraSqliteStatus execute(const Lease &lease, const void *sql, LyraSqliteError *error) {
    sqlite3_stmt *raw;
    auto status = prepareRaw(lease, sql, &raw, error, "exec");
    if (status != LYRA_SQLITE_OK) return status;
    if (sqlite3_bind_parameter_count(raw) || sqlite3_column_count(raw)) {
        status = problem(error, "exec", LYRA_SQLITE_INVALID, SQLITE_MISUSE, "exec requires a statement without parameters or result columns; use prepare() instead");
    } else {
        const auto code = sqlite3_step(raw);
        if (code != SQLITE_DONE) status = database(error, "exec", lease.connection->raw, code);
    }
    sqlite3_finalize(raw);
    return status;
}

LyraSqliteStatus lastInsertRowid(const Lease &lease, int64_t *output, LyraSqliteError *error) {
    auto status = access(lease, error, "lastInsertRowid");
    if (status == LYRA_SQLITE_OK) *output = sqlite3_last_insert_rowid(lease.connection->raw);
    return status;
}

LyraSqliteStatus finalize(LyraSqliteStatement *statement, LyraSqliteError *error) {
    const int code = sqlite3_finalize(statement->raw);
    statement->raw = nullptr;
    if (auto *transaction = statement->transaction) {
        if (statement->previous) statement->previous->next = statement->next;
        else transaction->statements = statement->next;
        if (statement->next) statement->next->previous = statement->previous;
        statement->previous = statement->next = nullptr;
    }
    return code == SQLITE_OK || statement->cursor == Cursor::Failed ? LYRA_SQLITE_OK
        : database(error, "finalize", statement->owner->raw, code);
}

int denyTransactionControl(void *, int action, const char *, const char *, const char *, const char *) {
    return action == SQLITE_TRANSACTION || action == SQLITE_SAVEPOINT ? SQLITE_DENY : SQLITE_OK;
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
    return prepare(Lease(connection), sql, output, error);
}

LyraSqliteStatus lyraSqliteExec(LyraSqliteConnection *connection, const void *sql, LyraSqliteError *error) {
    return execute(Lease(connection), sql, error);
}

LyraSqliteStatus lyraSqliteLastInsertRowid(LyraSqliteConnection *connection, int64_t *output, LyraSqliteError *error) {
    return lastInsertRowid(Lease(connection), output, error);
}

LyraSqliteStatus lyraSqliteBegin(LyraSqliteConnection *connection, LyraSqliteTransaction **output, LyraSqliteError *error) {
    *output = nullptr;
    Lease lease(connection);
    auto status = access(lease, error, "begin");
    if (status != LYRA_SQLITE_OK) return status;
    auto *raw = connection->raw;
    if (sqlite3_next_stmt(raw, nullptr))
        return problem(error, "begin", LYRA_SQLITE_BUSY, SQLITE_BUSY, "close existing statements before starting a transaction");
    if (!sqlite3_get_autocommit(raw))
        return problem(error, "begin", LYRA_SQLITE_INVALID, SQLITE_MISUSE, "a manually opened transaction is already active");
    auto code = sqlite3_exec(raw, "BEGIN", nullptr, nullptr, nullptr);
    if (code != SQLITE_OK) return database(error, "begin", raw, code);
    if (sqlite3_set_authorizer(raw, denyTransactionControl, nullptr) != SQLITE_OK) std::abort();
    *output = ::new (lyraGcAlloc(sizeof(LyraSqliteTransaction))) LyraSqliteTransaction{connection, &currentThread};
    lease.release = false;
    return LYRA_SQLITE_OK;
}

LyraSqliteStatus lyraSqliteFinish(LyraSqliteTransaction *transaction, int commit, LyraSqliteError *error, LyraSqliteIssue *cleanup) {
    *cleanup = {};
    auto *connection = transaction->owner;
    Lease lease(connection, transaction);
    if (lease.status == LYRA_SQLITE_CLOSED) return LYRA_SQLITE_OK;
    if (lease.status != LYRA_SQLITE_OK) return unavailable(error, commit ? "commit" : "rollback", lease.status);
    auto *raw = connection->raw;
    auto status = LYRA_SQLITE_OK;
    while (transaction->statements) {
        LyraSqliteError released;
        auto current = finalize(transaction->statements, &released);
        if (status == LYRA_SQLITE_OK && current != LYRA_SQLITE_OK) { status = current; *error = released; }
    }
    if (sqlite3_set_authorizer(raw, nullptr, nullptr) != SQLITE_OK) std::abort();
    if (commit && status == LYRA_SQLITE_OK) {
        if (sqlite3_get_autocommit(raw)) {
            status = problem(error, "commit", LYRA_SQLITE_DB_ERROR, SQLITE_ABORT, "SQLite has already rolled back this transaction");
        } else {
            auto code = sqlite3_exec(raw, "COMMIT", nullptr, nullptr, nullptr);
            if (code != SQLITE_OK) status = database(error, "commit", raw, code);
        }
    }
    if ((!commit || status != LYRA_SQLITE_OK) && !sqlite3_get_autocommit(raw)) {
        auto code = sqlite3_exec(raw, "ROLLBACK", nullptr, nullptr, nullptr);
        if (code != SQLITE_OK) {
            LyraSqliteIssue failure;
            failure.status = database(&failure.error, "rollback", raw, code);
            if (status == LYRA_SQLITE_OK) { status = failure.status; *error = failure.error; }
            else *cleanup = failure;
            sqlite3_close_v2(raw);
            connection->raw = nullptr;
        }
    }
    transaction->active.store(false, std::memory_order_release);
    connection->busy.clear(std::memory_order_release);
    return status;
}

LyraSqliteStatus lyraSqliteTransactionPrepare(LyraSqliteTransaction *transaction, const void *sql, LyraSqliteStatement **output, LyraSqliteError *error) {
    return prepare(Lease(transaction->owner, transaction), sql, output, error);
}

LyraSqliteStatus lyraSqliteTransactionExec(LyraSqliteTransaction *transaction, const void *sql, LyraSqliteError *error) {
    return execute(Lease(transaction->owner, transaction), sql, error);
}

LyraSqliteStatus lyraSqliteTransactionLastInsertRowid(LyraSqliteTransaction *transaction, int64_t *output, LyraSqliteError *error) {
    return lastInsertRowid(Lease(transaction->owner, transaction), output, error);
}

LyraSqliteStatus lyraSqliteBindInt(LyraSqliteStatement *statement, int64_t position, int64_t value, LyraSqliteError *error) {
    Lease lease(statement);
    auto status = bindPosition(lease, statement, position, error, "bindInt");
    if (status != LYRA_SQLITE_OK) return status;
    auto code = sqlite3_bind_int64(statement->raw, static_cast<int>(position), value);
    return code == SQLITE_OK ? LYRA_SQLITE_OK : database(error, "bindInt", statement->owner->raw, code);
}

LyraSqliteStatus lyraSqliteBindText(LyraSqliteStatement *statement, int64_t position, const void *value, LyraSqliteError *error) {
    Lease lease(statement);
    auto status = bindPosition(lease, statement, position, error, "bindText");
    if (status != LYRA_SQLITE_OK) return status;
    auto code = sqlite3_bind_text64(statement->raw, static_cast<int>(position), lyraStringBytes(value), lyraStringLength(value), SQLITE_TRANSIENT, SQLITE_UTF8);
    return code == SQLITE_OK ? LYRA_SQLITE_OK : database(error, "bindText", statement->owner->raw, code);
}

LyraSqliteStatus lyraSqliteBindFloat(LyraSqliteStatement *statement, int64_t position, double value, LyraSqliteError *error) {
    Lease lease(statement);
    auto status = bindPosition(lease, statement, position, error, "bindFloat");
    if (status != LYRA_SQLITE_OK) return status;
    if (std::isnan(value)) return problem(error, "bindFloat", LYRA_SQLITE_INVALID, SQLITE_MISMATCH, "SQLite cannot bind NaN as a REAL value");
    auto code = sqlite3_bind_double(statement->raw, static_cast<int>(position), value);
    return code == SQLITE_OK ? LYRA_SQLITE_OK : database(error, "bindFloat", statement->owner->raw, code);
}

LyraSqliteStatus lyraSqliteBindBlob(LyraSqliteStatement *statement, int64_t position, const void *value, LyraSqliteError *error) {
    Lease lease(statement);
    auto status = bindPosition(lease, statement, position, error, "bindBlob");
    if (status != LYRA_SQLITE_OK) return status;
    auto code = sqlite3_bind_blob64(statement->raw, static_cast<int>(position), lyraStringBytes(value), lyraStringLength(value), SQLITE_TRANSIENT);
    return code == SQLITE_OK ? LYRA_SQLITE_OK : database(error, "bindBlob", statement->owner->raw, code);
}

LyraSqliteStatus lyraSqliteBindNull(LyraSqliteStatement *statement, int64_t position, LyraSqliteError *error) {
    Lease lease(statement);
    auto status = bindPosition(lease, statement, position, error, "bindNull");
    if (status != LYRA_SQLITE_OK) return status;
    auto code = sqlite3_bind_null(statement->raw, static_cast<int>(position));
    return code == SQLITE_OK ? LYRA_SQLITE_OK : database(error, "bindNull", statement->owner->raw, code);
}

LyraSqliteStatus lyraSqliteNext(LyraSqliteStatement *statement, LyraSqliteError *error) {
    Lease lease(statement);
    auto status = access(lease, statement, error, "next");
    if (status != LYRA_SQLITE_OK) return status;
    if (statement->cursor == Cursor::Done) return LYRA_SQLITE_DONE;
    if (statement->cursor == Cursor::Failed) {
        *error = statement->failure.error;
        return statement->failure.status;
    }
    auto code = sqlite3_step(statement->raw);
    if (code == SQLITE_ROW || code == SQLITE_DONE) {
        statement->cursor = code == SQLITE_ROW ? Cursor::Row : Cursor::Done;
        return code == SQLITE_ROW ? LYRA_SQLITE_ROW : LYRA_SQLITE_DONE;
    }
    statement->cursor = Cursor::Failed;
    auto failed = database(error, "next", statement->owner->raw, code);
    statement->failure = {failed, *error};
    return failed;
}

LyraSqliteStatus lyraSqliteReadInt(LyraSqliteStatement *statement, int64_t column, int64_t *output, LyraSqliteError *error) {
    Lease lease(statement);
    auto status = columnType(lease, statement, column, SQLITE_INTEGER, error, "readInt");
    if (status == LYRA_SQLITE_OK) *output = sqlite3_column_int64(statement->raw, static_cast<int>(column));
    return status;
}

LyraSqliteStatus lyraSqliteReadText(LyraSqliteStatement *statement, int64_t column, const void **output, LyraSqliteError *error) {
    *output = nullptr;
    Lease lease(statement);
    auto status = columnType(lease, statement, column, SQLITE_TEXT, error, "readText");
    if (status != LYRA_SQLITE_OK) return status;
    auto index = static_cast<int>(column);
    auto *bytes = sqlite3_column_text(statement->raw, index);
    if (!bytes) return database(error, "readText", statement->owner->raw, SQLITE_NOMEM);
    int count = sqlite3_column_bytes(statement->raw, index);
    *output = lyraStringFromBytes(reinterpret_cast<const char *>(bytes), static_cast<size_t>(count));
    return LYRA_SQLITE_OK;
}

LyraSqliteStatus lyraSqliteReadFloat(LyraSqliteStatement *statement, int64_t column, double *output, LyraSqliteError *error) {
    Lease lease(statement);
    auto status = columnType(lease, statement, column, SQLITE_FLOAT, error, "readFloat");
    if (status == LYRA_SQLITE_OK) *output = sqlite3_column_double(statement->raw, static_cast<int>(column));
    return status;
}

LyraSqliteStatus lyraSqliteReadBlob(LyraSqliteStatement *statement, int64_t column, const void **output, LyraSqliteError *error) {
    *output = nullptr;
    Lease lease(statement);
    auto status = columnType(lease, statement, column, SQLITE_BLOB, error, "readBlob");
    if (status != LYRA_SQLITE_OK) return status;
    auto index = static_cast<int>(column);
    auto *bytes = sqlite3_column_blob(statement->raw, index);
    int count = sqlite3_column_bytes(statement->raw, index);
    if (!bytes && (count != 0 || sqlite3_errcode(statement->owner->raw) == SQLITE_NOMEM))
        return database(error, "readBlob", statement->owner->raw, SQLITE_NOMEM);
    *output = lyraStringFromBytes(static_cast<const char *>(bytes), static_cast<size_t>(count));
    return LYRA_SQLITE_OK;
}

LyraSqliteStatus lyraSqliteFinalize(LyraSqliteStatement *statement, LyraSqliteError *error) {
    Lease lease(statement);
    if (lease.status == LYRA_SQLITE_CLOSED) return LYRA_SQLITE_OK;
    if (lease.status != LYRA_SQLITE_OK) return unavailable(error, "finalize", lease.status);
    return statement->raw ? finalize(statement, error) : LYRA_SQLITE_OK;
}

LyraSqliteStatus lyraSqliteClose(LyraSqliteConnection *connection, LyraSqliteError *error) {
    Lease lease(connection);
    if (lease.status != LYRA_SQLITE_OK) return unavailable(error, "close", lease.status);
    if (!connection->raw) return LYRA_SQLITE_OK;
    int code = sqlite3_close(connection->raw);
    if (code != SQLITE_OK) return database(error, "close", connection->raw, code);
    connection->raw = nullptr;
    return LYRA_SQLITE_OK;
}
