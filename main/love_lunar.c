// main/love_lunar.c —— 农历换算实现。
//
// 表格式见 love_lunar_table.h。核心是一段"从锚点逐年逐月累加天数"的走法,
// 数据量小、逻辑直白,且能被 host tests 用已知节日日期逐条钉住。
#include "love_lunar.h"

#include "love_lunar_table.h"

#include <stdio.h>

// 该农历年的闰月号(0 = 无闰月)
int love_lunar_leap_month(int lunar_year)
{
    if (lunar_year < LOVE_LUNAR_BASE_YEAR ||
        lunar_year >= LOVE_LUNAR_BASE_YEAR + LOVE_LUNAR_YEARS) {
        return 0;
    }
    return (int)(LOVE_LUNAR_INFO[lunar_year - LOVE_LUNAR_BASE_YEAR] & 0x0F);
}

// 普通月(month = 1..12)的天数;闰月用 love_lunar_leap_days()。
static int plain_month_days(int lunar_year, int month)
{
    const uint32_t info = LOVE_LUNAR_INFO[lunar_year - LOVE_LUNAR_BASE_YEAR];
    return (info & (1u << (4 + month - 1))) ? 30 : 29;
}

// 闰月天数:表里每年只存一个 bit(该年闰月是 30 天还是 29 天),与闰月号无关。
static int leap_days_of(int lunar_year)
{
    const uint32_t info = LOVE_LUNAR_INFO[lunar_year - LOVE_LUNAR_BASE_YEAR];
    return (info & (1u << 16)) ? 30 : 29;
}

int love_lunar_month_days(int lunar_year, int lunar_month)
{
    if (lunar_year < LOVE_LUNAR_BASE_YEAR ||
        lunar_year >= LOVE_LUNAR_BASE_YEAR + LOVE_LUNAR_YEARS) {
        return 0;
    }
    if (lunar_month < 1 || lunar_month > 12) return 0;
    return plain_month_days(lunar_year, lunar_month);
}

// 整年天数 = 12 个普通月 + 闰月(若有)。腊月最后一天(除夕)要靠它推。
static int lunar_year_days(int lunar_year)
{
    int days = 0;
    for (int m = 1; m <= 12; m++) days += plain_month_days(lunar_year, m);
    const int leap = love_lunar_leap_month(lunar_year);
    if (leap != 0) days += leap_days_of(lunar_year);
    return days;
}

// 从锚点(LOVE_LUNAR_BASE_YEAR 正月初一)数到该农历年正月初一的天数。
static int days_before_lunar_year(int lunar_year)
{
    int days = 0;
    for (int y = LOVE_LUNAR_BASE_YEAR; y < lunar_year; y++) days += lunar_year_days(y);
    return days;
}

// 该农历年正月初一到 (month, day) 的天数偏移(不含 day 本身)。
// day == 0 表示取该月最后一天。
static bool lunar_day_offset(int lunar_year, int lunar_month, int lunar_day, int *offset)
{
    if (lunar_month < 1 || lunar_month > 12) return false;

    const int leap = love_lunar_leap_month(lunar_year);
    int days = 0;
    for (int m = 1; m < lunar_month; m++) {
        days += plain_month_days(lunar_year, m);
        if (leap == m) days += leap_days_of(lunar_year);   // 闰月排在其月之后
    }

    const int this_month = plain_month_days(lunar_year, lunar_month);
    const int day = (lunar_day == 0) ? this_month : lunar_day;
    if (day < 1 || day > this_month) return false;

    *offset = days + day - 1;
    return true;
}

bool love_lunar_to_solar(int lunar_year, int lunar_month, int lunar_day, love_date_t *out)
{
    if (!out) return false;
    if (lunar_year < LOVE_LUNAR_BASE_YEAR ||
        lunar_year >= LOVE_LUNAR_BASE_YEAR + LOVE_LUNAR_YEARS) {
        return false;
    }

    int offset = 0;
    if (!lunar_day_offset(lunar_year, lunar_month, lunar_day, &offset)) return false;

    const int64_t anchor = love_days_from_civil(
        (love_date_t){ LOVE_LUNAR_BASE_YEAR, LOVE_LUNAR_BASE_MONTH, LOVE_LUNAR_BASE_DAY });
    *out = love_civil_from_days(anchor + days_before_lunar_year(lunar_year) + offset);
    return true;
}

bool love_lunar_next_occurrence(love_date_t today, int lunar_month, int lunar_day,
                                love_date_t *out)
{
    if (!out) return false;

    // 农历年号与公历年的关系:正月落在公历年当年(1~2 月),腊月可能落到次年。
    // 所以从 today.year - 1 起试三年足够覆盖。
    bool found = false;
    love_date_t best = { 0, 0, 0 };

    for (int ly = today.year - 1; ly <= today.year + 1; ly++) {
        love_date_t candidate;
        if (!love_lunar_to_solar(ly, lunar_month, lunar_day, &candidate)) continue;
        if (love_days_between(today, candidate) < 0) continue;   // 已经过了
        if (!found || love_days_between(best, candidate) < 0) {
            best = candidate;
            found = true;
        }
    }
    if (!found) return false;

    *out = best;
    return true;
}

/* ---------- 可读文本 ---------- */

static const char *const MONTH_NAMES[13] = {
    "", "正月", "二月", "三月", "四月", "五月", "六月",
    "七月", "八月", "九月", "十月", "冬月", "腊月",
};

static const char *const DAY_TENS[4] = { "初", "十", "廿", "三" };
static const char *const DAY_UNITS[10] = {
    "十", "一", "二", "三", "四", "五", "六", "七", "八", "九",
};

// 农历日的中文写法:初一..初十、十一..十九、二十、廿一..廿九、三十
static void format_day(int day, char *buf, size_t size)
{
    if (day == 10) { snprintf(buf, size, "初十"); return; }
    if (day == 20) { snprintf(buf, size, "二十"); return; }
    if (day == 30) { snprintf(buf, size, "三十"); return; }
    snprintf(buf, size, "%s%s", DAY_TENS[day / 10], DAY_UNITS[day % 10]);
}

void love_lunar_format(int lunar_month, int lunar_day, char *buf, size_t size)
{
    if (!buf || size == 0) return;
    if (lunar_month < 1 || lunar_month > 12) {
        snprintf(buf, size, "农历");
        return;
    }
    if (lunar_day == 0) {
        snprintf(buf, size, "%s最后一天", MONTH_NAMES[lunar_month]);
        return;
    }
    if (lunar_day < 1 || lunar_day > 30) {
        snprintf(buf, size, "%s", MONTH_NAMES[lunar_month]);
        return;
    }

    char day[8];
    format_day(lunar_day, day, sizeof(day));
    snprintf(buf, size, "%s%s", MONTH_NAMES[lunar_month], day);
}

// 农历日的可编辑范围:0(月末)再加 1..30，共 31 个取值。农历月最长 30 天。
#define LUNAR_DAY_STEPS 31

void love_lunar_step(int *lunar_month, int *lunar_day, int field, int delta)
{
    if (!lunar_month || !lunar_day) return;

    if (field == 0) {
        int month = *lunar_month + delta;
        while (month < 1) month += 12;
        while (month > 12) month -= 12;
        *lunar_month = month;
    } else {
        // 0..30 环绕:0 是"月末",1..30 是具体日子。这样设备上也能直接调出除夕。
        int day = *lunar_day + delta;
        while (day < 0) day += LUNAR_DAY_STEPS;
        while (day >= LUNAR_DAY_STEPS) day -= LUNAR_DAY_STEPS;
        *lunar_day = day;
    }
}
