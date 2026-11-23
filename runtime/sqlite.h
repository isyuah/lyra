#ifndef LYRA_RUNTIME_SQLITE_H
#define LYRA_RUNTIME_SQLITE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LyraSqliteConnection LyraSqliteConnection;
typedef struct LyraSqliteStatement LyraSqliteStatement;
typedef struct LyraSqliteError {
    const char *operation;
    const void *message;
    int code;
} LyraSqliteError;

typedef enum LyraSqliteStatus {
    LYRA_SQLITE_OK, LYRA_SQLITE_ROW, LYRA_SQLITE_DONE, LYRA_SQLITE_NULL,
    LYRA_SQLITE_BUSY, LYRA_SQLITE_CLOSED, LYRA_SQLITE_DB_ERROR, LYRA_SQLITE_INVALID
} LyraSqliteStatus;

LyraSqliteStatus lyraSqliteOpen(const void *path, int readOnly, LyraSqliteConnection **output, LyraSqliteError *error);
LyraSqliteStatus lyraSqlitePrepare(LyraSqliteConnection *connection, const void *sql, LyraSqliteStatement **output, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteBindInt(LyraSqliteStatement *statement, int position, int64_t value, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteBindText(LyraSqliteStatement *statement, int position, const void *value, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteNext(LyraSqliteStatement *statement, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteReadInt(LyraSqliteStatement *statement, int column, int64_t *output, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteReadText(LyraSqliteStatement *statement, int column, const void **output, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteFinalize(LyraSqliteStatement *statement, LyraSqliteError *error);
LyraSqliteStatus lyraSqliteClose(LyraSqliteConnection *connection, LyraSqliteError *error);

#ifdef __cplusplus
}
#endif

#endif
