// main/love_date.c —— love_date.h 的实现,只做纯计算,不碰硬件。
#include "love_date.h"

#include "love_lunar.h"

#include <stdio.h>

// 公历换算采用 Howard Hinnant 的 days_from_civil / civil_from_days,
// 对 1970 之前(负纪元天数)同样成立。
int64_t love_days_from_civil(love_date_t d)
{
    int y = d.year;
    const unsigned m = (unsigned)d.month;
    const unsigned dd = (unsigned)d.day;
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - (int)(era * 400));
    const unsigned doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + dd - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

love_date_t love_civil_from_days(int64_t days)
{
    int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int y = (int)yoe + (int)(era * 400);
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    const unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned m = mp + (mp < 10u ? 3u : (unsigned)-9);
    const int year = y + ((m <= 2u) ? 1 : 0);

    love_date_t out = { .year = (int16_t)year, .month = (int8_t)m, .day = (int8_t)d };
    return out;
}

bool love_is_leap(int year)
{
    if (year % 4 != 0) return false;
    if (year % 100 != 0) return true;
    return year % 400 == 0;
}

int love_days_in_month(int year, int month)
{
    static const int8_t LENGTHS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 0;
    if (month == 2 && love_is_leap(year)) return 29;
    return LENGTHS[month - 1];
}

bool love_date_valid(love_date_t d)
{
    if (d.year < 1970 || d.year > 2099) return false;
    if (d.month < 1 || d.month > 12) return false;
    if (d.day < 1) return false;
    return d.day <= love_days_in_month(d.year, d.month);
}

int32_t love_days_between(love_date_t from, love_date_t to)
{
    return (int32_t)(love_days_from_civil(to) - love_days_from_civil(from));
}

int32_t love_days_together(love_date_t start, love_date_t today)
{
    int32_t diff = love_days_between(start, today);
    // 起始日当天算第 1 天;今天早于起始日时没有“在一起天数”。
    if (diff < 0) return 0;
    return diff + 1;
}

love_date_t love_next_occurrence(love_date_t today, int month, int day)
{
    if (month < 1 || month > 12) month = 1;

    // 目标日超出当月天数(2 月 29 日遇平年)时折算到当月最后一天。
    int max_day = love_days_in_month(today.year, month);
    int use_day = day > max_day ? max_day : day;
    if (use_day < 1) use_day = 1;

    love_date_t candidate = { today.year, (int8_t)month, (int8_t)use_day };
    if (love_days_between(today, candidate) >= 0) return candidate;

    int next_year = today.year + 1;
    int next_max = love_days_in_month(next_year, month);
    int next_day = day > next_max ? next_max : day;
    if (next_day < 1) next_day = 1;
    love_date_t next = { (int16_t)next_year, (int8_t)month, (int8_t)next_day };
    return next;
}

love_countdown_t love_event_countdown(const love_event_t *event, love_date_t today)
{
    love_countdown_t out = { 0, true, true, today };
    if (!event) return out;

    if (event->kind == LOVE_EVENT_LUNAR) {
        // 农历节日的目标日要靠农历表算;表覆盖不到的年份明确标成未解析,
        // 而不是拿一个假日期糊过去。
        love_date_t target;
        if (!love_lunar_next_occurrence(today, event->date.month, event->date.day, &target)) {
            out.resolved = false;
            out.target = event->date;
            out.days = 0;
            out.upcoming = true;
            return out;
        }
        out.target = target;
    } else if (event->kind == LOVE_EVENT_YEARLY) {
        out.target = love_next_occurrence(today, event->date.month, event->date.day);
    } else {
        out.target = event->date;
    }
    out.days = love_days_between(today, out.target);
    out.upcoming = out.days >= 0;
    return out;
}

love_date_t love_date_step(love_date_t d, int field, int delta)
{
    if (!love_date_valid(d)) return d;

    if (field == LOVE_FIELD_YEAR) {
        int year = d.year + delta;
        if (year < 1970) year = 1970;
        if (year > 2099) year = 2099;
        d.year = (int16_t)year;
    } else if (field == LOVE_FIELD_MONTH) {
        int month = d.month + delta;
        while (month < 1) month += 12;
        while (month > 12) month -= 12;
        d.month = (int8_t)month;
    } else if (field == LOVE_FIELD_DAY) {
        int length = love_days_in_month(d.year, d.month);
        int day = d.day + delta;
        while (day < 1) day += length;
        while (day > length) day -= length;
        d.day = (int8_t)day;
    }

    // 年份变化后当月天数可能变短(2 月),把日期夹回合法范围。
    int length = love_days_in_month(d.year, d.month);
    if (d.day > length) d.day = (int8_t)length;
    return d;
}

void love_date_format(love_date_t d, char *buf, size_t size)
{
    if (!buf || size == 0) return;
    if (!love_date_valid(d)) {
        buf[0] = '\0';
        return;
    }
    snprintf(buf, size, "%04d-%02d-%02d", (int)d.year, (int)d.month, (int)d.day);
}

bool love_date_parse(const char *text, love_date_t *out)
{
    if (!text || !out) return false;
    int year = 0, month = 0, day = 0;
    if (sscanf(text, "%4d-%2d-%2d", &year, &month, &day) != 3) return false;
    love_date_t date = { (int16_t)year, (int8_t)month, (int8_t)day };
    if (!love_date_valid(date)) return false;
    *out = date;
    return true;
}

love_date_t love_date_from_epoch(uint64_t epoch_seconds, int32_t tz_offset_seconds)
{
    int64_t shifted = (int64_t)epoch_seconds + (int64_t)tz_offset_seconds;
    // 负值(1970 前)也能正确向下取整。
    int64_t days = shifted >= 0 ? shifted / 86400 : (shifted - 86399) / 86400;
    return love_civil_from_days(days);
}

void love_hms_from_epoch(uint64_t epoch_seconds, int32_t tz_offset_seconds,
                         int *hour, int *minute, int *second)
{
    int64_t shifted = (int64_t)epoch_seconds + (int64_t)tz_offset_seconds;
    int64_t seconds_of_day = shifted % 86400;
    if (seconds_of_day < 0) seconds_of_day += 86400;
    if (hour) *hour = (int)(seconds_of_day / 3600);
    if (minute) *minute = (int)((seconds_of_day % 3600) / 60);
    if (second) *second = (int)(seconds_of_day % 60);
}
