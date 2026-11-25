#ifndef LYRA_RUNTIME_SQLITE_H
#define LYRA_RUNTIME_SQLITE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LyraSqliteConnection LyraSqliteConnection;
typedef struct LyraSqliteStatement LyraSqliteStatement;
typedef struct LyraSqliteTransaction LyraSqliteTransaction;
typedef struct LyraSqliteError {
    const char *operation;
    const void *message;
    int code;
} LyraSqliteError;

typedef enum LyraSqliteStatus {
    LYRA_SQLITE_OK, LYRA_SQLITE_ROW, LYRA_SQLITE_DONE, LYRA_SQLITE_NULL,
    LYRA_SQLITE_BUSY, LYRA_SQLITE_CLOSED, LYRA_SQLITE_DB_ERROR, LYRA_SQLITE_INVALID
} LyraSqliteStatus;

typedef struct LyraSqliteIssue {
    LyraSqliteStatus status;
    LyraSqliteError error;
} LyraSqliteIssue;

LyraSqliteStatus lyraSqliteOpen(const void *path, int readOnly, LyraSqliteConnection **output, LyraSqliteError *error);
LyraSqliteStatus lyraSqlitePrepare(LyraSqliteConnection *connection, const void *sql, LyraSqliteStatement **output, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteExec(LyraSqliteConnection *connection, const void *sql, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteLastInsertRowid(LyraSqliteConnection *connection, int64_t *output, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteBegin(LyraSqliteConnection *connection, LyraSqliteTransaction **output, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteFinish(LyraSqliteTransaction *transaction, int commit, LyraSqliteError *error, LyraSqliteIssue *cleanup);
LyraSqliteStatus lyraSqliteTransactionPrepare(LyraSqliteTransaction *transaction, const void *sql, LyraSqliteStatement **output, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteTransactionExec(LyraSqliteTransaction *transaction, const void *sql, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteTransactionLastInsertRowid(LyraSqliteTransaction *transaction, int64_t *output, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteBindInt(LyraSqliteStatement *statement, int64_t position, int64_t value, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteBindText(LyraSqliteStatement *statement, int64_t position, const void *value, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteBindFloat(LyraSqliteStatement *statement, int64_t position, double value, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteBindBlob(LyraSqliteStatement *statement, int64_t position, const void *value, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteBindNull(LyraSqliteStatement *statement, int64_t position, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteNext(LyraSqliteStatement *statement, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteReadInt(LyraSqliteStatement *statement, int64_t column, int64_t *output, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteReadText(LyraSqliteStatement *statement, int64_t column, const void **output, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteReadFloat(LyraSqliteStatement *statement, int64_t column, double *output, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteReadBlob(LyraSqliteStatement *statement, int64_t column, const void **output, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteFinalize(LyraSqliteStatement *statement, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteClose(LyraSqliteConnection *connection, LyraSqliteError *error);

#ifdef __cplusplus
}
#endif

#endif
