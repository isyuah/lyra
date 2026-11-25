# std.time：日期、墙钟和单调钟

- `Clock.now()` 返回 UTC `DateTime`。用 `.formatUtc()` 输出日志时间，`.unixMs()` 取 Unix 毫秒。
- `Clock.monotonicMs()` 用于耗时和截止时间，起点由系统决定。不要把这个值当作 Unix 时间；异步延时沿用冷任务 `sleepMs`。
- `Date.parse("2024-02-29")` 检查 `YYYY-MM-DD` 和闰年。`DateTime.parse("2024-02-29T10:30:40.12+08:00")` 支持显式 `Z` 或 `±HH:MM`，小数秒最多九位。`date()` 和 `toString()` 使用原偏移，`formatUtc()` 转成 UTC。
- 两种解析返回 `Result`。日期采用公历的 `0001`–`9999` 年；不接受闰秒、无偏移时间、表示未知偏移的 `-00:00` 或区域时区名称。
