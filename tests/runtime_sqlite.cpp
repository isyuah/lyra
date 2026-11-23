#include "managed.h"
#include "sqlite.h"
#include "strings.h"
#include "tasks.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

void check(bool value, const char *message) {
    if (!value) {
        fprintf(stderr, "sqlite test: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

const void *text(const char *bytes) { return lyraStringFromBytes(bytes, strlen(bytes)); }

void matches(const void *string, const char *bytes, size_t length) {
    check(string && lyraStringLength(string) == length && memcmp(lyraStringBytes(string), bytes, length) == 0, "copied text or length changed");
}

LyraSqliteConnection *openMemory() {
    LyraSqliteConnection *connection;
    LyraSqliteError error;
    check(lyraSqliteOpen(text(":memory:"), 0, &connection, &error) == LYRA_SQLITE_OK, "open failed");
    return connection;
}

LyraSqliteStatement *prepare(LyraSqliteConnection *connection, const char *sql) {
    LyraSqliteStatement *statement;
    LyraSqliteError error;
    check(lyraSqlitePrepare(connection, text(sql), &statement, &error) == LYRA_SQLITE_OK, "prepare failed");
    return statement;
}

void closeStatement(LyraSqliteStatement *statement) {
    LyraSqliteError error;
    check(lyraSqliteFinalize(statement, &error) == LYRA_SQLITE_OK, "finalize failed");
}

void closeConnection(LyraSqliteConnection *connection) {
    LyraSqliteError error;
    check(lyraSqliteClose(connection, &error) == LYRA_SQLITE_OK, "close failed");
}

void execute(LyraSqliteConnection *connection, const char *sql) {
    auto *statement = prepare(connection, sql);
    LyraSqliteError error;
    check(lyraSqliteNext(statement, &error) == LYRA_SQLITE_DONE, "command failed");
    closeStatement(statement);
}

void queryAndCleanup() {
    auto *connection = openMemory();
    auto *alias = connection;
    execute(connection, "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT)");
    LyraSqliteError error;
    auto *insert = prepare(connection, "INSERT INTO users(id,name) VALUES(?1,?2)");
    check(lyraSqliteBindInt(insert, 1, 7, &error) == LYRA_SQLITE_OK, "bind id failed");
    check(lyraSqliteBindText(insert, 2, text("Ada"), &error) == LYRA_SQLITE_OK, "bind name failed");
    check(lyraSqliteNext(insert, &error) == LYRA_SQLITE_DONE && lyraSqliteNext(insert, &error) == LYRA_SQLITE_DONE, "DONE reran the insert");
    closeStatement(insert);
    auto *query = prepare(alias, "SELECT id, name, ?2, NULL, '' FROM users WHERE id=?1");
    auto *queryAlias = query;
    const char data[] = {'x', 0, '\xe4', '\xb8', '\xad'};
    check(lyraSqliteBindInt(query, 1, 7, &error) == LYRA_SQLITE_OK, "bind lookup failed");
    check(lyraSqliteBindText(query, 2, lyraStringFromBytes(data, sizeof(data)), &error) == LYRA_SQLITE_OK, "bind counted text failed");
    check(lyraSqliteClose(connection, &error) == LYRA_SQLITE_BUSY, "connection with a statement was closed");
    const void *name;
    const void *other;
    const void *empty;
    const void *absent;
    int64_t id;
    check(lyraSqliteReadText(query, 1, &name, &error) == LYRA_SQLITE_INVALID, "read before first row succeeded");
    check(lyraSqliteNext(query, &error) == LYRA_SQLITE_ROW, "missing requested row");
    check(lyraSqliteReadInt(query, 0, &id, &error) == LYRA_SQLITE_OK && id == 7, "wrong integer result");
    check(lyraSqliteReadText(query, 1, &name, &error) == LYRA_SQLITE_OK && lyraSqliteReadText(query, 2, &other, &error) == LYRA_SQLITE_OK, "text column failed");
    check(lyraSqliteReadText(query, 3, &absent, &error) == LYRA_SQLITE_NULL && !absent, "SQL NULL became empty text");
    check(lyraSqliteReadText(query, 4, &empty, &error) == LYRA_SQLITE_OK, "empty text became SQL NULL");
    check(lyraSqliteReadText(query, 9, &absent, &error) == LYRA_SQLITE_INVALID, "out-of-range read succeeded");
    check(lyraSqliteNext(query, &error) == LYRA_SQLITE_DONE && lyraSqliteNext(query, &error) == LYRA_SQLITE_DONE, "cursor unexpectedly restarted");
    closeStatement(query);
    closeStatement(queryAlias);
    check(lyraSqliteNext(queryAlias, &error) == LYRA_SQLITE_CLOSED, "statement alias used stale handle");

    LyraSqliteStatement *missing;
    check(lyraSqlitePrepare(connection, text("SELECT * FROM missing_table"), &missing, &error) == LYRA_SQLITE_DB_ERROR && !missing, "missing table did not fail");
    const auto prepareError = error;
    auto *badInsert = prepare(connection, "INSERT INTO users VALUES(7,'duplicate')");
    check(lyraSqliteNext(badInsert, &error) == LYRA_SQLITE_DB_ERROR, "step error became DONE");
    check(lyraSqliteFinalize(badInsert, &error) == LYRA_SQLITE_DB_ERROR, "finalize lost prior SQLite step error");
    closeStatement(badInsert);
    closeConnection(connection);
    closeConnection(alias);
    check(lyraSqlitePrepare(alias, text("SELECT 1"), &missing, &error) == LYRA_SQLITE_CLOSED, "connection alias used stale handle");
    lyraGcCollect();
    check(prepareError.code != 0 && strcmp(prepareError.operation, "prepare") == 0 && lyraStringLength(prepareError.message) > 0, "lost copied SQLite cause after close");
    matches(name, "Ada", 3);
    matches(other, data, sizeof(data));
    matches(empty, "", 0);
}

struct Gate {
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
    bool atQuery = false;
    bool released = false;
};

struct Job {
    Gate *gate;
    LyraSqliteConnection *connection;
    LyraSqliteStatement *statement;
    const void *name;
    bool closed;
    bool submitted;
};

void nativeQuery(void *storage) {
    auto *job = static_cast<Job *>(storage);
    job->connection = openMemory();
    job->statement = prepare(job->connection, "SELECT 'Ada'");
    auto *gate = job->gate;
    pthread_mutex_lock(&gate->lock);
    gate->atQuery = true;
    pthread_cond_signal(&gate->changed);
    while (!gate->released) pthread_cond_wait(&gate->changed, &gate->lock);
    pthread_mutex_unlock(&gate->lock);

    LyraSqliteError error;
    check(lyraSqliteNext(job->statement, &error) == LYRA_SQLITE_ROW, "cancel closed statement before native step");
    check(lyraSqliteReadText(job->statement, 0, &job->name, &error) == LYRA_SQLITE_OK, "worker failed to copy text");
    closeStatement(job->statement);
    closeConnection(job->connection);
    job->closed = true;
}

void queryTask(LyraTask *task, void *storage) {
    auto *job = static_cast<Job *>(storage);
    if (!job->submitted) {
        job->submitted = true;
        lyraTaskBlock(task, nativeQuery, job);
        return;
    }
    check(job->closed && lyraTaskCancelRequested(task), "cancel resumed before native cleanup");
    lyraTaskFinishCancelled(task);
}

struct Disconnect { Job *job; LyraTask *query; };

void disconnectTask(LyraTask *task, void *storage) {
    auto *event = static_cast<Disconnect *>(storage);
    auto *gate = event->job->gate;
    pthread_mutex_lock(&gate->lock);
    while (!gate->atQuery) pthread_cond_wait(&gate->changed, &gate->lock);
    lyraTaskRequestCancel(event->query);
    check(lyraTaskState(event->query) == LYRA_TASK_SUSPENDED && !event->job->closed, "cancellation released active native resources");
    LyraSqliteError error;
    check(lyraSqliteClose(event->job->connection, &error) == LYRA_SQLITE_BUSY, "close should report active statement");
    lyraGcCollect();
    gate->released = true;
    pthread_cond_signal(&gate->changed);
    pthread_mutex_unlock(&gate->lock);
    lyraTaskComplete(task, nullptr);
}

}

int main() {
    alarm(20);
    queryAndCleanup();
    auto *domain = lyraDomainCreate();
    Gate gate;
    auto *job = static_cast<Job *>(lyraGcAlloc(sizeof(Job)));
    job->gate = &gate;
    auto *query = lyraTaskCreate(queryTask, job);
    Disconnect event{job, query};
    lyraTaskStart(domain, query);
    lyraTaskStart(domain, lyraTaskCreate(disconnectTask, &event));
    lyraDomainRun(domain);
    check(lyraTaskState(query) == LYRA_TASK_CANCELLED && job->closed, "cancelled task did not finish after safe close");
    lyraGcCollect();
    matches(job->name, "Ada", 3);
    lyraDomainDestroy(domain);
    pthread_cond_destroy(&gate.changed);
    pthread_mutex_destroy(&gate.lock);
}
