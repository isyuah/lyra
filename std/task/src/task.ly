extern fn lyraContextCurrent(): CPtr;
extern fn lyraContextWithTrace(context: CPtr, trace: String): CPtr;
extern fn lyraContextWithTimeout(context: CPtr, milliseconds: Int): CPtr;
extern fn lyraContextTrace(context: CPtr): String;
extern fn lyraContextRemaining(context: CPtr): Int;
extern fn lyraContextEnter(context: CPtr): Bool;

pub fn trace(): String { lyraContextTrace(lyraContextCurrent()) }
pub fn remainingMs(): Int { lyraContextRemaining(lyraContextCurrent()) }

pub async fn withTrace<T>(label: String, body: Task<T>): T {
    lyraContextEnter(lyraContextWithTrace(lyraContextCurrent(), label))
    await body
}

pub async fn withTimeout<T>(milliseconds: Int, body: Task<T>): T {
    lyraContextEnter(lyraContextWithTimeout(lyraContextCurrent(), milliseconds))
    await body
}
