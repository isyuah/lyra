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

/// 公历日期或带偏移量的日期时间无法解析。
pub enum TimeError {
    /// 不符合 `YYYY-MM-DD` 或日期不在公历有效范围内。
    InvalidDate,
    /// 日期时间格式无效，包括缺少明确 UTC 偏移或出现闰秒。
    InvalidDateTime,
}

/// 不含时间与时区的公历日期，年份范围为 `0001`–`9999`。
pub struct Date(val packed: Int) {
    /// 四位公历年。
    pub val year: Int get() { packed / 10000 }
    /// 月份从 `1` 到 `12`。
    pub val month: Int get() { packed / 100 % 100 }
    /// 当月中的日，从 `1` 开始。
    pub val day: Int get() { packed % 100 }

    /// 解析严格的 `YYYY-MM-DD`，检查闰年和每月天数。
    pub static fn parse(value: String): Result<Date, TimeError> {
        val packed = lyraDateParse(value)
        if packed == 0 { Err(TimeError.InvalidDate) } else { Ok(unpackDate(packed)) }
    }

    /// 格式化为补零的 `YYYY-MM-DD`。
    pub fn toString(): String { "${pad(year, 4)}-${pad(month, 2)}-${pad(day, 2)}" }
}

fn pad(value: Int, width: Int): String {
    var text = value.toString()
    while text.size < width { text = "0" + text }
    text
}

fn unpackDate(value: Int): Date { Date(value) }

/// 精确到纳秒的时间点，同时记录用于显示的 UTC 偏移量，不包含地区时区规则。
pub struct DateTime(val seconds: Int, val nanos: Int, val offset: Int) {
    /// 距 Unix 纪元的整秒数，与显示偏移无关。
    pub val unixSeconds: Int get() { seconds }
    /// 秒内的纳秒分量，范围为 `0` 到 `999999999`。
    pub val nanosecond: Int get() { nanos }
    /// 显示时间相对 UTC 的带符号分钟数；UTC 为 `0`。
    pub val offsetMinutes: Int get() { offset }

    /// 解析带 `Z` 或 `±HH:MM` 的日期时间，接受最多九位小数秒，不接受 `-00:00` 或时区名。
    pub static fn parse(value: String): Result<DateTime, TimeError> {
        val parsed = lyraDateTimeParse(value)
        if lyraTimeValid(parsed) { Ok(unpackTime(parsed)) } else { Err(TimeError.InvalidDateTime) }
    }

    /// 按保存的 UTC 偏移量取当地公历日期。
    pub fn date(): Date { unpackDate(lyraTimeLocalDate(unixSeconds, offsetMinutes)) }
    /// 返回 Unix 毫秒数，丢弃不足一毫秒的纳秒部分。
    pub fn unixMs(): Int { unixSeconds * 1000 + nanosecond / 1000000 }
    /// 按 UTC 格式化时间点，以 `Z` 表示偏移。
    pub fn formatUtc(): String { lyraTimeFormat(unixSeconds, nanosecond, 0) }
    /// 按保存的偏移量格式化；偏移为零时使用 `Z`。
    pub fn toString(): String { lyraTimeFormat(unixSeconds, nanosecond, offsetMinutes) }
}

fn unpackTime(value: CPtr): DateTime {
    DateTime(lyraTimeSeconds(value), lyraTimeNanoseconds(value), lyraTimeOffset(value))
}

/// 系统墙钟和单调时钟的入口。
pub class Clock {
    /// 读取当前 UTC 时间；系统墙钟不可用时抛出异常。
    pub static fn now(): DateTime {
        val value = lyraTimeNow()
        if !lyraTimeValid(value) { throw Exception("UTC clock unavailable") }
        unpackTime(value)
    }

    /// 读取单调时钟毫秒数，适合计算耗时或期限，起点不是 Unix 纪元。
    pub static fn monotonicMs(): Int { lyraContextClock() }
}
