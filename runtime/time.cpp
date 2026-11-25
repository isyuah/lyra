#include "managed.h"
#include "strings.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <uv.h>

namespace {

using namespace std::chrono;

struct Timestamp { std::int64_t seconds, nanoseconds, offsetMinutes; };

const Timestamp* timestamp(Timestamp value) {
    auto* result = static_cast<Timestamp*>(lyraGcAllocAtomic(sizeof(Timestamp)));
    *result = value;
    return result;
}

int digits(const char* text, int count) {
    int value = 0;
    for (int index = 0; index < count; ++index) {
        if (text[index] < '0' || text[index] > '9') return -1;
        value = value * 10 + text[index] - '0';
    }
    return value;
}

year_month_day parseDate(const char* value) {
    if (value[4] != '-' || value[7] != '-') return {};
    auto parsedYear = digits(value, 4);
    auto parsedMonth = digits(value + 5, 2);
    auto parsedDay = digits(value + 8, 2);
    if (parsedYear <= 0 || parsedMonth <= 0 || parsedDay <= 0) return {};
    return year{parsedYear} / static_cast<unsigned>(parsedMonth) / static_cast<unsigned>(parsedDay);
}

std::int64_t packedDate(year_month_day date) {
    return static_cast<int>(date.year()) * 10000 + static_cast<unsigned>(date.month()) * 100 + static_cast<unsigned>(date.day());
}

sys_seconds localTime(std::int64_t epochSeconds, std::int64_t offsetMinutes) { return sys_seconds{seconds{epochSeconds}} + minutes{offsetMinutes}; }

}

extern "C" std::int64_t lyraDateParse(const void* value) {
    if (lyraStringLength(value) != 10) return 0;
    auto date = parseDate(lyraStringBytes(value));
    return date.ok() ? packedDate(date) : 0;
}

extern "C" const Timestamp* lyraDateTimeParse(const void* value) {
    auto length = lyraStringLength(value);
    auto text = lyraStringBytes(value);
    if (length < 20 || length > 35 || text[10] != 'T' || text[13] != ':' || text[16] != ':') return nullptr;
    auto date = parseDate(text);
    auto hour = digits(text + 11, 2), minute = digits(text + 14, 2), second = digits(text + 17, 2);
    if (!date.ok() || hour < 0 || hour >= 24 || minute < 0 || minute >= 60 || second < 0 || second >= 60) return nullptr;
    std::size_t cursor = 19;
    std::int64_t nanoseconds = 0;
    if (text[cursor] == '.') {
        auto begin = ++cursor;
        while (cursor < length && text[cursor] >= '0' && text[cursor] <= '9') ++cursor;
        auto count = cursor - begin;
        if (count == 0 || count > 9 || cursor == length) return nullptr;
        nanoseconds = digits(text + begin, static_cast<int>(count));
        for (; count < 9; ++count) nanoseconds *= 10;
    }
    int offset = 0;
    if (text[cursor] == 'Z' && length - cursor == 1) {
        offset = 0;
    } else {
        auto sign = text[cursor];
        if ((sign != '+' && sign != '-') || length - cursor != 6 || text[cursor + 3] != ':') return nullptr;
        auto hours = digits(text + cursor + 1, 2), minutes = digits(text + cursor + 4, 2);
        if (hours < 0 || hours >= 24 || minutes < 0 || minutes >= 60) return nullptr;
        offset = hours * 60 + minutes;
        if (sign == '-') {
            if (offset == 0) return nullptr;
            offset = -offset;
        }
    }
    auto whole = sys_days{date} + hours{hour} + minutes{minute} + seconds{second} - minutes{offset};
    auto utcDate = year_month_day{floor<days>(whole)};
    if (utcDate.year() < year{1} || utcDate.year() > year{9999}) return nullptr;
    return timestamp({whole.time_since_epoch().count(), nanoseconds, offset});
}

extern "C" const Timestamp* lyraTimeNow() {
    uv_timeval64_t value;
    if (uv_gettimeofday(&value) != 0) return nullptr;
    return timestamp({value.tv_sec, value.tv_usec * 1000, 0});
}

extern "C" std::uint8_t lyraTimeValid(const Timestamp* value) { return value != nullptr; }
extern "C" std::int64_t lyraTimeSeconds(const Timestamp* value) { return value->seconds; }
extern "C" std::int64_t lyraTimeNanoseconds(const Timestamp* value) { return value->nanoseconds; }
extern "C" std::int64_t lyraTimeOffset(const Timestamp* value) { return value->offsetMinutes; }

extern "C" std::int64_t lyraTimeLocalDate(std::int64_t seconds, std::int64_t offsetMinutes) {
    return packedDate(year_month_day{floor<days>(localTime(seconds, offsetMinutes))});
}

extern "C" const void* lyraTimeFormat(std::int64_t seconds, std::int64_t nanoseconds, std::int64_t offsetMinutes) {
    auto timestamp = localTime(seconds, offsetMinutes);
    auto boundary = floor<days>(timestamp);
    auto date = year_month_day{boundary};
    auto time = hh_mm_ss{timestamp - boundary};
    char text[64];
    auto size = std::snprintf(text, sizeof(text), "%04d-%02u-%02uT%02d:%02d:%02d",
        static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
        static_cast<int>(time.hours().count()), static_cast<int>(time.minutes().count()), static_cast<int>(time.seconds().count()));
    if (nanoseconds != 0) {
        size += std::snprintf(text + size, sizeof(text) - size, ".%09d", static_cast<int>(nanoseconds));
        while (text[size - 1] == '0') --size;
    }
    if (offsetMinutes == 0) text[size++] = 'Z';
    else {
        auto magnitude = static_cast<int>(offsetMinutes < 0 ? -offsetMinutes : offsetMinutes);
        size += std::snprintf(text + size, sizeof(text) - size, "%c%02d:%02d", offsetMinutes < 0 ? '-' : '+', magnitude / 60, magnitude % 60);
    }
    return lyraStringFromBytes(text, size);
}
