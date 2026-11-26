extern fn lyraDbOpen(path: String, readOnly: Bool): CPtr;
extern fn lyraDbPrepare(db: CPtr, sql: String): CPtr;
extern fn lyraDbExec(db: CPtr, sql: String): CPtr;
extern fn lyraDbLastInsertRowid(db: CPtr): CPtr;
extern fn lyraDbBegin(db: CPtr): CPtr;
extern fn lyraDbFinish(scope: CPtr, commit: Bool): CPtr;
extern fn lyraDbTransactionPrepare(scope: CPtr, sql: String): CPtr;
extern fn lyraDbTransactionExec(scope: CPtr, sql: String): CPtr;
extern fn lyraDbTransactionLastInsertRowid(scope: CPtr): CPtr;
extern fn lyraDbClose(db: CPtr): CPtr;
extern fn lyraDbFinalize(cursor: CPtr): CPtr;
extern fn lyraDbNext(cursor: CPtr): CPtr;
extern fn lyraDbBindInt(cursor: CPtr, position: Int, number: Int): CPtr;
extern fn lyraDbBindText(cursor: CPtr, position: Int, text: String): CPtr;
extern fn lyraDbBindFloat(cursor: CPtr, position: Int, value: Float64): CPtr;
extern fn lyraDbBindBlob(cursor: CPtr, position: Int, value: Bytes): CPtr;
extern fn lyraDbBindNull(cursor: CPtr, position: Int): CPtr;
extern fn lyraDbIntAt(cursor: CPtr, column: Int): CPtr;
extern fn lyraDbTextAt(cursor: CPtr, column: Int): CPtr;
extern fn lyraDbFloatAt(cursor: CPtr, column: Int): CPtr;
extern fn lyraDbBlobAt(cursor: CPtr, column: Int): CPtr;
extern fn lyraDbStatus(result: CPtr): Int;
extern fn lyraDbCode(result: CPtr): Int;
extern fn lyraDbNumber(result: CPtr): Int;
extern fn lyraDbFloat(result: CPtr): Float64;
extern fn lyraDbText(result: CPtr): String;
extern fn lyraDbBlob(result: CPtr): Bytes;
extern fn lyraDbMessage(result: CPtr): String;
extern fn lyraDbOperation(result: CPtr): String;
extern fn lyraDbHasCleanup(result: CPtr): Bool;
extern fn lyraDbCleanup(result: CPtr): CPtr;

pub enum DbErrorKind { Busy, Closed, Sqlite, Invalid }

pub struct DbError(pub val kind: DbErrorKind, pub val code: Int, pub val operation: String, val detail: String, pub val cleanupError: Error? = None) {
    pub fn message(): String { detail }
    pub static fn typeMismatch(operation: String, message: String): DbError {
        DbError(DbErrorKind.Invalid, 20, operation, message)
    }
    fn withCleanup(failure: DbError): DbError {
        val extra = Error(failure)
        val cleanup = match cleanupError { Some(previous) => previous.withSuppressed(extra), None => extra }
        DbError(kind, code, operation, detail, Some(cleanup))
    }
}

pub exception Busy
pub exception CloseError

fn dbError(result: CPtr): DbError {
    val kind = match lyraDbStatus(result) {
        4 => DbErrorKind.Busy,
        5 => DbErrorKind.Closed,
        6 => DbErrorKind.Sqlite,
        7 => DbErrorKind.Invalid,
        _ => throw Exception("unexpected SQLite error status"),
    }
    DbError(kind, lyraDbCode(result), lyraDbOperation(result), lyraDbMessage(result))
}

fn command(result: CPtr): Result<Unit, DbError> {
    if lyraDbStatus(result) == 0 { Ok({}) } else { Err(dbError(result)) }
}

fn statement(result: CPtr): Result<Statement, DbError> {
    if lyraDbStatus(result) == 0 { Ok(Statement(result)) } else { Err(dbError(result)) }
}

fn rowid(result: CPtr): Result<Int, DbError> {
    if lyraDbStatus(result) == 0 { Ok(lyraDbNumber(result)) } else { Err(dbError(result)) }
}

fn checkClose(result: CPtr): Unit {
    val status = lyraDbStatus(result)
    if status == 0 { return }
    val message = lyraDbOperation(result) + ": " + lyraDbMessage(result)
    if status == 4 { throw Busy(message) }
    throw CloseError(message)
}

pub class Connection {
    val handle: CPtr
    init(handle: CPtr) { this.handle = handle }

    pub fn prepare(sql: String): Result<Statement, DbError> { statement(lyraDbPrepare(handle, sql)) }
    pub fn exec(sql: String): Result<Unit, DbError> { command(lyraDbExec(handle, sql)) }
    pub fn lastInsertRowid(): Result<Int, DbError> { rowid(lyraDbLastInsertRowid(handle)) }
    pub fn transaction<T>(body: (Transaction) -> Result<T, DbError>): Result<T, DbError> {
        val started = lyraDbBegin(handle)
        command(started)?
        defer checkClose(lyraDbFinish(started, false))
        val outcome = body(Transaction(started))
        val commit = match outcome { Ok(_) => true, Err(_) => false }
        val finished = lyraDbFinish(started, commit)
        if lyraDbStatus(finished) == 0 { return outcome }
        val finishError = dbError(finished)
        val primary = match outcome { Ok(_) => finishError, Err(cause) => cause.withCleanup(finishError) }
        if lyraDbHasCleanup(finished) { Err(primary.withCleanup(dbError(lyraDbCleanup(finished)))) } else { Err(primary) }
    }
    pub fn close(): Unit { checkClose(lyraDbClose(handle)) }
}

pub class Transaction {
    val handle: CPtr
    init(handle: CPtr) { this.handle = handle }

    pub fn prepare(sql: String): Result<Statement, DbError> { statement(lyraDbTransactionPrepare(handle, sql)) }
    pub fn exec(sql: String): Result<Unit, DbError> { command(lyraDbTransactionExec(handle, sql)) }
    pub fn lastInsertRowid(): Result<Int, DbError> { rowid(lyraDbTransactionLastInsertRowid(handle)) }
}

pub class Statement {
    val handle: CPtr
    init(handle: CPtr) { this.handle = handle }

    pub fn bindInt(position: Int, value: Int): Result<Unit, DbError> { command(lyraDbBindInt(handle, position, value)) }
    pub fn bindText(position: Int, value: String): Result<Unit, DbError> { command(lyraDbBindText(handle, position, value)) }
    pub fn bindFloat(position: Int, value: Float64): Result<Unit, DbError> { command(lyraDbBindFloat(handle, position, value)) }
    pub fn bindBlob(position: Int, value: Bytes): Result<Unit, DbError> { command(lyraDbBindBlob(handle, position, value)) }
    pub fn bindNull(position: Int): Result<Unit, DbError> { command(lyraDbBindNull(handle, position)) }
    pub fn next(): Result<Bool, DbError> {
        val result = lyraDbNext(handle)
        match lyraDbStatus(result) {
            1 => Ok(true),
            2 => Ok(false),
            _ => Err(dbError(result)),
        }
    }
    pub fn readText(column: Int): Result<String?, DbError> {
        val result = lyraDbTextAt(handle, column)
        match lyraDbStatus(result) {
            0 => Ok(Some(lyraDbText(result))),
            3 => Ok(None),
            _ => Err(dbError(result)),
        }
    }
    pub fn readInt(column: Int): Result<Int?, DbError> {
        val result = lyraDbIntAt(handle, column)
        match lyraDbStatus(result) {
            0 => Ok(Some(lyraDbNumber(result))),
            3 => Ok(None),
            _ => Err(dbError(result)),
        }
    }
    pub fn readFloat(column: Int): Result<Float64?, DbError> {
        val result = lyraDbFloatAt(handle, column)
        match lyraDbStatus(result) {
            0 => Ok(Some(lyraDbFloat(result))),
            3 => Ok(None),
            _ => Err(dbError(result)),
        }
    }
    pub fn readBlob(column: Int): Result<Bytes?, DbError> {
        val result = lyraDbBlobAt(handle, column)
        match lyraDbStatus(result) {
            0 => Ok(Some(lyraDbBlob(result))),
            3 => Ok(None),
            _ => Err(dbError(result)),
        }
    }
    pub fn close(): Unit { checkClose(lyraDbFinalize(handle)) }
}

fn connect(path: String, readOnly: Bool): Result<Connection, DbError> {
    val result = lyraDbOpen(path, readOnly)
    if lyraDbStatus(result) == 0 { Ok(Connection(result)) } else { Err(dbError(result)) }
}

pub fn open(path: String): Result<Connection, DbError> { connect(path, false) }
pub fn openReadOnly(path: String): Result<Connection, DbError> { connect(path, true) }

pub fn withConnection<T>(path: String, readOnly: Bool = false, body: (Connection) -> Result<T, DbError>): Result<T, DbError> {
    val connection = connect(path, readOnly)?
    defer connection.close()
    body(connection)
}
