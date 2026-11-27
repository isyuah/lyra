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

/// 连接、语句或 SQLite 操作的失败分类。
pub enum DbErrorKind {
    /// 句柄正在使用或 SQLite 报告锁冲突。
    Busy,
    /// 对应连接、语句或事务已经结束。
    Closed,
    /// SQLite 返回了其他执行错误。
    Sqlite,
    /// 无效的操作、索引、存储类型或其他输入。
    Invalid,
}

/// 可传播的 SQLite 业务错误，保留原生信息与可选的事务清理失败。
pub struct DbError(
    /// 用于判断是否可重试或应修正入参的错误分类。
    pub val kind: DbErrorKind,
    /// SQLite 或适配层使用的错误码。
    pub val code: Int,
    /// 报告错误的操作名。
    pub val operation: String, val detail: String,
    /// 在处理主错误时附加的清理失败；主错误仍保留在其余字段。
    pub val cleanupError: Error? = None
) {
    /// 返回失败详情，操作名可从 `operation` 单独读取。
    pub fn message(): String { detail }
    /// 为应用自己的列或结果类型约束构造 `Invalid` 错误，错误码为 `SQLITE_MISMATCH`。
    pub static fn typeMismatch(operation: String, message: String): DbError {
        DbError(DbErrorKind.Invalid, 20, operation, message)
    }
    fn withCleanup(failure: DbError): DbError {
        val extra = Error(failure)
        val cleanup = match cleanupError { Some(previous) => previous.withSuppressed(extra), None => extra }
        DbError(kind, code, operation, detail, Some(cleanup))
    }
}

/// 关闭连接或语句时仍有占用；关闭语句或等占用操作完成后可重试。
pub exception Busy
/// 除句柄占用以外的 SQLite 清理失败。
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

/// 同步 SQLite 连接。异步应用应将查询和清理一起放入同一个 `blocking` 作用域。
pub class Connection {
    val handle: CPtr
    init(handle: CPtr) { this.handle = handle }

    /// 准备一条语句；用 `Statement` 绑定参数、逐行执行并显式关闭。
    pub fn prepare(sql: String): Result<Statement, DbError> { statement(lyraDbPrepare(handle, sql)) }
    /// 执行一条不接受绑定参数、不返回列的语句，例如建表；查询或 `RETURNING` 用 `prepare`。
    pub fn exec(sql: String): Result<Unit, DbError> { command(lyraDbExec(handle, sql)) }
    /// 读取当前连接最近一次成功插入的 rowid；SQLite 不会因随后回滚而重置它。
    pub fn lastInsertRowid(): Result<Int, DbError> { rowid(lyraDbLastInsertRowid(handle)) }
    /// 在同步回调中执行事务：返回 `Ok` 时提交，返回 `Err` 或抛异常时回滚，退出前关闭事务语句。
    /// 开始前须关闭连接上已有的语句；回调内用传入的 `Transaction`，不能嵌套同连接的事务。
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
    /// 关闭连接，对所有别名生效；有未关闭的语句或占用时抛出 `Busy`，成功后可重复关闭。
    pub fn close(): Unit { checkClose(lyraDbClose(handle)) }
}

/// 连接独占事务的操作入口，仅在回调线程与事务存续期间有效。
pub class Transaction {
    val handle: CPtr
    init(handle: CPtr) { this.handle = handle }

    /// 准备一条事务内语句；若不提前关闭，事务结束时会一并清理。
    pub fn prepare(sql: String): Result<Statement, DbError> { statement(lyraDbTransactionPrepare(handle, sql)) }
    /// 执行一条不返回列、不绑定参数的语句；不能手动控制事务或保存点。
    pub fn exec(sql: String): Result<Unit, DbError> { command(lyraDbTransactionExec(handle, sql)) }
    /// 在事务仍持有连接时读取最近一次插入的 rowid，便于与该事务内的写入关联。
    pub fn lastInsertRowid(): Result<Int, DbError> { rowid(lyraDbTransactionLastInsertRowid(handle)) }
}

/// 独立的预编译语句与行游标。参数从 `1` 开始编号，结果列从 `0` 开始。
pub class Statement {
    val handle: CPtr
    init(handle: CPtr) { this.handle = handle }

    /// 在指定的一基占位符绑定 SQLite `INTEGER`。
    pub fn bindInt(position: Int, value: Int): Result<Unit, DbError> { command(lyraDbBindInt(handle, position, value)) }
    /// 在指定的一基占位符绑定 `TEXT`；空字符串不是 SQL `NULL`。
    pub fn bindText(position: Int, value: String): Result<Unit, DbError> { command(lyraDbBindText(handle, position, value)) }
    /// 在指定的一基占位符绑定 `REAL`；拒绝会被 SQLite 当作 `NULL` 的 NaN。
    pub fn bindFloat(position: Int, value: Float64): Result<Unit, DbError> { command(lyraDbBindFloat(handle, position, value)) }
    /// 在指定的一基占位符绑定 `BLOB`；空字节串仍是 BLOB。
    pub fn bindBlob(position: Int, value: Bytes): Result<Unit, DbError> { command(lyraDbBindBlob(handle, position, value)) }
    /// 在指定的一基占位符显式绑定 SQL `NULL`。
    pub fn bindNull(position: Int): Result<Unit, DbError> { command(lyraDbBindNull(handle, position)) }
    /// 推进语句；当前有可读行时返回 `true`，执行完成时返回 `false`。
    pub fn next(): Result<Bool, DbError> {
        val result = lyraDbNext(handle)
        match lyraDbStatus(result) {
            1 => Ok(true),
            2 => Ok(false),
            _ => Err(dbError(result)),
        }
    }
    /// 读取当前行的 `TEXT` 列，得到独立副本。`NULL` 返回 `None`，其他存储类型不做转型。
    pub fn readText(column: Int): Result<String?, DbError> {
        val result = lyraDbTextAt(handle, column)
        match lyraDbStatus(result) {
            0 => Ok(Some(lyraDbText(result))),
            3 => Ok(None),
            _ => Err(dbError(result)),
        }
    }
    /// 读取当前行的 `INTEGER` 列；`NULL` 返回 `None`，不做类型转换。
    pub fn readInt(column: Int): Result<Int?, DbError> {
        val result = lyraDbIntAt(handle, column)
        match lyraDbStatus(result) {
            0 => Ok(Some(lyraDbNumber(result))),
            3 => Ok(None),
            _ => Err(dbError(result)),
        }
    }
    /// 读取当前行的 `REAL` 列；`NULL` 返回 `None`，不会把整数列隐式转成浮点。
    pub fn readFloat(column: Int): Result<Float64?, DbError> {
        val result = lyraDbFloatAt(handle, column)
        match lyraDbStatus(result) {
            0 => Ok(Some(lyraDbFloat(result))),
            3 => Ok(None),
            _ => Err(dbError(result)),
        }
    }
    /// 复制当前行的 `BLOB` 字节；`NULL` 返回 `None`，空 BLOB 返回 `Some`。
    pub fn readBlob(column: Int): Result<Bytes?, DbError> {
        val result = lyraDbBlobAt(handle, column)
        match lyraDbStatus(result) {
            0 => Ok(Some(lyraDbBlob(result))),
            3 => Ok(None),
            _ => Err(dbError(result)),
        }
    }
    /// 释放语句及游标，使别名也失效；已由 `next()` 报告的执行错误不会重复抛出。
    pub fn close(): Unit { checkClose(lyraDbFinalize(handle)) }
}

fn connect(path: String, readOnly: Bool): Result<Connection, DbError> {
    val result = lyraDbOpen(path, readOnly)
    if lyraDbStatus(result) == 0 { Ok(Connection(result)) } else { Err(dbError(result)) }
}

/// 以读写方式打开数据库，路径不存在时创建数据库文件。
pub fn open(path: String): Result<Connection, DbError> { connect(path, false) }
/// 以只读方式打开已有数据库，路径不存在时返回错误。
pub fn openReadOnly(path: String): Result<Connection, DbError> { connect(path, true) }

/// 打开连接、执行同步回调并返回其结果；失败或抛异常时也会尝试关闭连接。
pub fn withConnection<T>(path: String, readOnly: Bool = false, body: (Connection) -> Result<T, DbError>): Result<T, DbError> {
    val connection = connect(path, readOnly)?
    defer connection.close()
    body(connection)
}
