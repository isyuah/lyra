extern fn lyraDbOpen(path: String, readOnly: Bool): CPtr;
extern fn lyraDbPrepare(db: CPtr, sql: String): CPtr;
extern fn lyraDbClose(db: CPtr): CPtr;
extern fn lyraDbFinalize(cursor: CPtr): CPtr;
extern fn lyraDbNext(cursor: CPtr): CPtr;
extern fn lyraDbBindInt(cursor: CPtr, position: Int, number: Int): CPtr;
extern fn lyraDbBindText(cursor: CPtr, position: Int, text: String): CPtr;
extern fn lyraDbIntAt(cursor: CPtr, column: Int): CPtr;
extern fn lyraDbTextAt(cursor: CPtr, column: Int): CPtr;
extern fn lyraDbStatus(result: CPtr): Int;
extern fn lyraDbCode(result: CPtr): Int;
extern fn lyraDbNumber(result: CPtr): Int;
extern fn lyraDbText(result: CPtr): String;
extern fn lyraDbMessage(result: CPtr): String;
extern fn lyraDbOperation(result: CPtr): String;

pub enum DbErrorKind { Busy, Closed, Sqlite, Invalid }

pub struct DbError(pub val kind: DbErrorKind, pub val code: Int, pub val operation: String, val detail: String) {
    pub fn message(): String { detail }
    pub static fn typeMismatch(operation: String, message: String): DbError {
        DbError(DbErrorKind.Invalid, 20, operation, message)
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

fn bound(result: CPtr): Result<Unit, DbError> {
    if lyraDbStatus(result) == 0 { Ok({}) } else { Err(dbError(result)) }
}

fn checkClose(result: CPtr): Unit {
    val status = lyraDbStatus(result)
    if status == 0 { return }
    val message = lyraDbOperation(result) + ": " + lyraDbMessage(result)
    if status == 4 { throw Busy(message) }
    throw CloseError(message)
}

pub class Connection(val handle: CPtr) {
    pub fn prepare(sql: String): Result<Statement, DbError> {
        val result = lyraDbPrepare(handle, sql)
        if lyraDbStatus(result) == 0 { Ok(Statement(result)) } else { Err(dbError(result)) }
    }
    pub fn close(): Unit { checkClose(lyraDbClose(handle)) }
}

pub class Statement(val handle: CPtr) {
    pub fn bindInt(position: Int, value: Int): Result<Unit, DbError> { bound(lyraDbBindInt(handle, position, value)) }
    pub fn bindText(position: Int, value: String): Result<Unit, DbError> { bound(lyraDbBindText(handle, position, value)) }
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
    pub fn close(): Unit { checkClose(lyraDbFinalize(handle)) }
}

fn connect(path: String, readOnly: Bool): Result<Connection, DbError> {
    val result = lyraDbOpen(path, readOnly)
    if lyraDbStatus(result) == 0 { Ok(Connection(result)) } else { Err(dbError(result)) }
}

pub fn open(path: String): Result<Connection, DbError> { connect(path, false) }
pub fn openReadOnly(path: String): Result<Connection, DbError> { connect(path, true) }
