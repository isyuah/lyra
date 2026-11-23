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

pub struct DbStatus(val handle: CPtr) {
    pub fn ok(): Bool { lyraDbStatus(handle) == 0 }
    pub fn row(): Bool { lyraDbStatus(handle) == 1 }
    pub fn done(): Bool { lyraDbStatus(handle) == 2 }
    pub fn isNull(): Bool { lyraDbStatus(handle) == 3 }
    pub fn busy(): Bool { lyraDbStatus(handle) == 4 }
    pub fn code(): Int { lyraDbCode(handle) }
    pub fn message(): String { lyraDbMessage(handle) }
    pub fn operation(): String { lyraDbOperation(handle) }
}

pub struct DbText(pub val status: DbStatus, pub val value: String)
pub struct DbInt(pub val status: DbStatus, pub val value: Int)

pub class Connection(val handle: CPtr) {
    pub fn status(): DbStatus { DbStatus(handle) }
    pub fn prepare(sql: String): Statement { Statement(lyraDbPrepare(handle, sql)) }
    pub fn close(): DbStatus { DbStatus(lyraDbClose(handle)) }
}

pub class Statement(val handle: CPtr) {
    pub fn status(): DbStatus { DbStatus(handle) }
    pub fn bindInt(position: Int, value: Int): DbStatus { DbStatus(lyraDbBindInt(handle, position, value)) }
    pub fn bindText(position: Int, value: String): DbStatus { DbStatus(lyraDbBindText(handle, position, value)) }
    pub fn next(): DbStatus { DbStatus(lyraDbNext(handle)) }
    pub fn readText(column: Int): DbText {
        val read = lyraDbTextAt(handle, column)
        DbText(DbStatus(read), lyraDbText(read))
    }
    pub fn readInt(column: Int): DbInt {
        val read = lyraDbIntAt(handle, column)
        DbInt(DbStatus(read), lyraDbNumber(read))
    }
    pub fn close(): DbStatus { DbStatus(lyraDbFinalize(handle)) }
}

pub fn open(path: String): Connection { Connection(lyraDbOpen(path, false)) }
pub fn openReadOnly(path: String): Connection { Connection(lyraDbOpen(path, true)) }
