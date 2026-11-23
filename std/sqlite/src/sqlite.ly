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
    pub fn ok(self): Bool { lyraDbStatus(self.handle) == 0 }
    pub fn row(self): Bool { lyraDbStatus(self.handle) == 1 }
    pub fn done(self): Bool { lyraDbStatus(self.handle) == 2 }
    pub fn isNull(self): Bool { lyraDbStatus(self.handle) == 3 }
    pub fn busy(self): Bool { lyraDbStatus(self.handle) == 4 }
    pub fn code(self): Int { lyraDbCode(self.handle) }
    pub fn message(self): String { lyraDbMessage(self.handle) }
    pub fn operation(self): String { lyraDbOperation(self.handle) }
}

pub struct DbText(pub val status: DbStatus, pub val value: String)
pub struct DbInt(pub val status: DbStatus, pub val value: Int)

pub class Connection(val handle: CPtr) {
    pub fn status(self): DbStatus { DbStatus(self.handle) }
    pub fn prepare(self, sql: String): Statement { Statement(lyraDbPrepare(self.handle, sql)) }
    pub fn close(self): DbStatus { DbStatus(lyraDbClose(self.handle)) }
}

pub class Statement(val handle: CPtr) {
    pub fn status(self): DbStatus { DbStatus(self.handle) }
    pub fn bindInt(self, position: Int, value: Int): DbStatus { DbStatus(lyraDbBindInt(self.handle, position, value)) }
    pub fn bindText(self, position: Int, value: String): DbStatus { DbStatus(lyraDbBindText(self.handle, position, value)) }
    pub fn next(self): DbStatus { DbStatus(lyraDbNext(self.handle)) }
    pub fn readText(self, column: Int): DbText {
        val read = lyraDbTextAt(self.handle, column)
        DbText(DbStatus(read), lyraDbText(read))
    }
    pub fn readInt(self, column: Int): DbInt {
        val read = lyraDbIntAt(self.handle, column)
        DbInt(DbStatus(read), lyraDbNumber(read))
    }
    pub fn close(self): DbStatus { DbStatus(lyraDbFinalize(self.handle)) }
}

pub fn open(path: String): Connection { Connection(lyraDbOpen(path, false)) }
pub fn openReadOnly(path: String): Connection { Connection(lyraDbOpen(path, true)) }
