extern fn lyraIoArgCount(): Int;
extern fn lyraIoArgument(index: Int): CPtr;
extern fn lyraIoReadLine(): CPtr;
extern fn lyraIoReadFile(path: String): CPtr;
extern fn lyraIoTextStatus(result: CPtr): Int;
extern fn lyraIoInvalidUtf8(status: Int): Bool;
extern fn lyraIoTextValue(result: CPtr): String;
extern fn lyraIoWriteFile(path: String, content: String): Int;
extern fn lyraIoWriteError(content: String): Int;
extern fn lyraIoAssert(condition: Bool, message: String): Unit;

pub struct TextRead(pub val status: Int, pub val value: String) {
    pub fn ok(self): Bool { self.status == 0 }
    pub fn eof(self): Bool { self.status == -1 }
    pub fn invalidUtf8(self): Bool { lyraIoInvalidUtf8(self.status) }
}

fn text(result: CPtr): TextRead { TextRead(lyraIoTextStatus(result), lyraIoTextValue(result)) }
pub fn argCount(): Int { lyraIoArgCount() }
pub fn arg(index: Int): TextRead { text(lyraIoArgument(index)) }
pub fn readLine(): TextRead { text(lyraIoReadLine()) }
pub fn readFile(path: String): TextRead { text(lyraIoReadFile(path)) }
pub fn writeFile(path: String, content: String): Int { lyraIoWriteFile(path, content) }
pub fn writeError(content: String): Int { lyraIoWriteError(content) }
pub fn assert(condition: Bool, message: String): Unit { lyraIoAssert(condition, message) }
