extern fn lyraDateParse(value: String): Int;
extern fn lyraDateTimeParse(value: String): CPtr;
extern fn lyraTimeNow(): CPtr;
extern fn lyraTimeValid(value: CPtr): Bool;
extern fn lyraTimeSeconds(value: CPtr): Int;
extern fn lyraTimeNanoseconds(value: CPtr): Int;
extern fn lyraTimeOffset(value: CPtr): Int;
extern fn lyraTimeLocalDate(seconds: Int, offsetMinutes: Int): Int;
extern fn lyraTimeFormat(seconds: Int, nanoseconds: Int, offsetMinutes: Int): String;
extern fn lyraContextClock(): Int;

pub enum TimeError { InvalidDate, InvalidDateTime }

pub struct Date(val packed: Int) {
    pub val year: Int get() { packed / 10000 }
    pub val month: Int get() { packed / 100 % 100 }
    pub val day: Int get() { packed % 100 }

    pub static fn parse(value: String): Result<Date, TimeError> {
        val packed = lyraDateParse(value)
        if packed == 0 { Err(TimeError.InvalidDate) } else { Ok(unpackDate(packed)) }
    }

    pub fn toString(): String { "${pad(year, 4)}-${pad(month, 2)}-${pad(day, 2)}" }
}

fn pad(value: Int, width: Int): String {
    var text = value.toString()
    while text.size < width { text = "0" + text }
    text
}

fn unpackDate(value: Int): Date { Date(value) }

pub struct DateTime(val seconds: Int, val nanos: Int, val offset: Int) {
    pub val unixSeconds: Int get() { seconds }
    pub val nanosecond: Int get() { nanos }
    pub val offsetMinutes: Int get() { offset }

    pub static fn parse(value: String): Result<DateTime, TimeError> {
        val parsed = lyraDateTimeParse(value)
        if lyraTimeValid(parsed) { Ok(unpackTime(parsed)) } else { Err(TimeError.InvalidDateTime) }
    }

    pub fn date(): Date { unpackDate(lyraTimeLocalDate(unixSeconds, offsetMinutes)) }
    pub fn unixMs(): Int { unixSeconds * 1000 + nanosecond / 1000000 }
    pub fn formatUtc(): String { lyraTimeFormat(unixSeconds, nanosecond, 0) }
    pub fn toString(): String { lyraTimeFormat(unixSeconds, nanosecond, offsetMinutes) }
}

fn unpackTime(value: CPtr): DateTime {
    DateTime(lyraTimeSeconds(value), lyraTimeNanoseconds(value), lyraTimeOffset(value))
}

pub class Clock {
    pub static fn now(): DateTime {
        val value = lyraTimeNow()
        if !lyraTimeValid(value) { throw Exception("UTC clock unavailable") }
        unpackTime(value)
    }

    pub static fn monotonicMs(): Int { lyraContextClock() }
}
