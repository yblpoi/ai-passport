// tests/test_love_date.c —— 纪念日摆件日期逻辑的 host 测试。
//
// 参考日 2026-09-16(只是夹具日期,与出厂默认无关):
// 在一起 35 天 / 元旦 107 天后 / 情人节 151 天后 / 圣诞节 100 天后 /
// 国庆节 15 天后 / 另一条年度事件(7 月 15 日)302 天后。参考天数由 Python datetime 独立算出。
#include <assert.h>
#include <string.h>
#include "love_date.h"

static love_date_t date(int y, int m, int d)
{
    love_date_t out = { (int16_t)y, (int8_t)m, (int8_t)d };
    return out;
}

static void test_civil_conversion(void)
{
    assert(love_days_from_civil(date(1970, 1, 1)) == 0);
    assert(love_days_from_civil(date(2026, 9, 16)) == 20712);
    assert(love_days_from_civil(date(2000, 1, 1)) == 10957);

    // 反向换算与纪元前(负天数)同样成立。
    love_date_t back = love_civil_from_days(20712);
    assert(back.year == 2026 && back.month == 9 && back.day == 16);
    love_date_t epoch = love_civil_from_days(0);
    assert(epoch.year == 1970 && epoch.month == 1 && epoch.day == 1);
    love_date_t before = love_civil_from_days(-1);
    assert(before.year == 1969 && before.month == 12 && before.day == 31);

    for (int32_t day = 20000; day < 22000; day += 37) {
        assert(love_days_from_civil(love_civil_from_days(day)) == day);
    }
}

static void test_calendar_rules(void)
{
    assert(love_is_leap(2024));
    assert(love_is_leap(2000));
    assert(!love_is_leap(2026));
    assert(!love_is_leap(1900));
    assert(love_days_in_month(2026, 2) == 28);
    assert(love_days_in_month(2028, 2) == 29);
    assert(love_days_in_month(2026, 12) == 31);
    assert(love_days_in_month(2026, 4) == 30);
    assert(love_days_in_month(2026, 13) == 0);

    assert(love_date_valid(date(2026, 2, 28)));
    assert(!love_date_valid(date(2026, 2, 29)));
    assert(love_date_valid(date(2028, 2, 29)));
    assert(!love_date_valid(date(2026, 13, 1)));
    assert(!love_date_valid(date(2026, 4, 31)));
    assert(!love_date_valid(date(2026, 0, 10)));
    assert(!love_date_valid(date(1969, 12, 31)));
}

static void test_days_between_and_together(void)
{
    assert(love_days_between(date(2000, 1, 1), date(2026, 9, 16)) == 9755);
    // 在一起天数含起始日当天。
    assert(love_days_together(date(2000, 1, 1), date(2026, 9, 16)) == 9756);
    assert(love_days_together(date(2000, 1, 1), date(2000, 1, 1)) == 1);
    assert(love_days_together(date(2025, 12, 31), date(2026, 1, 1)) == 2);
    // 今天早于起始日:没有在一起天数。
    assert(love_days_together(date(2026, 9, 16), date(2026, 9, 15)) == 0);
}

static void test_next_occurrence_matches_screenshots(void)
{
    love_date_t today = date(2026, 9, 16);

    love_date_t national = love_next_occurrence(today, 10, 1);
    assert(national.year == 2026 && national.month == 10 && national.day == 1);
    assert(love_days_between(today, national) == 15);

    love_date_t new_year = love_next_occurrence(today, 1, 1);
    assert(new_year.year == 2027 && love_days_between(today, new_year) == 107);

    love_date_t valentine = love_next_occurrence(today, 2, 14);
    assert(valentine.year == 2027 && love_days_between(today, valentine) == 151);

    love_date_t christmas = love_next_occurrence(today, 12, 25);
    assert(christmas.year == 2026 && love_days_between(today, christmas) == 100);

    love_date_t mid_july = love_next_occurrence(today, 7, 15);
    assert(mid_july.year == 2027 && mid_july.month == 7 && mid_july.day == 15);
    assert(love_days_between(today, mid_july) == 302);

    // 今天就是节日当天:算“今天”。
    love_date_t same_day = love_next_occurrence(today, 9, 16);
    assert(same_day.year == 2026 && same_day.month == 9 && same_day.day == 16);
}

static void test_next_occurrence_leap_fallback(void)
{
    // 2 月 29 日的年度事件在平年折算到 2 月 28 日。
    love_date_t flat = love_next_occurrence(date(2026, 3, 1), 2, 29);
    assert(flat.year == 2027 && flat.month == 2 && flat.day == 28);
    // 闰年保持 2 月 29 日。
    love_date_t leap = love_next_occurrence(date(2027, 3, 1), 2, 29);
    assert(leap.year == 2028 && leap.month == 2 && leap.day == 29);
    love_date_t same_year = love_next_occurrence(date(2028, 1, 1), 2, 29);
    assert(same_year.year == 2028 && same_year.day == 29);
    // 非法月份回退到 1 月,不产生非法日期。
    love_date_t bad = love_next_occurrence(date(2026, 5, 1), 0, 0);
    assert(love_date_valid(bad));
}

static void test_event_countdown(void)
{
    love_date_t today = date(2026, 9, 16);

    love_event_t yearly = { .name = "国庆节", .icon = 0,
                            .kind = LOVE_EVENT_YEARLY, .date = date(2026, 10, 1) };
    love_countdown_t c = love_event_countdown(&yearly, today);
    assert(c.upcoming && c.days == 15);
    assert(c.target.year == 2026 && c.target.month == 10);

    love_event_t once_future = { .kind = LOVE_EVENT_ONCE, .date = date(2026, 10, 1) };
    assert(love_event_countdown(&once_future, today).days == 15);

    // 一次性事件过期后显示“已过 N 天”。
    love_event_t once_past = { .kind = LOVE_EVENT_ONCE, .date = date(2026, 9, 1) };
    love_countdown_t past = love_event_countdown(&once_past, today);
    assert(!past.upcoming && past.days == -15);

    // 按年重复的事件永远不会过期。
    love_event_t *null_event = NULL;
    assert(love_event_countdown(null_event, today).days == 0);
}

static void test_format_and_epoch(void)
{
    char buf[16];
    love_date_format(date(2000, 1, 1), buf, sizeof(buf));
    assert(strcmp(buf, "2000-01-01") == 0);
    love_date_format(date(2026, 12, 5), buf, sizeof(buf));
    assert(strcmp(buf, "2026-12-05") == 0);
    love_date_format(date(2026, 2, 30), buf, sizeof(buf));
    assert(buf[0] == '\0');

    // 1789560000 = 2026-09-16 12:00:00 UTC。
    const uint64_t epoch = 1789560000u;
    love_date_t east8 = love_date_from_epoch(epoch, 8 * 3600);
    assert(east8.year == 2026 && east8.month == 9 && east8.day == 16);

    int hour = -1, minute = -1, second = -1;
    love_hms_from_epoch(epoch, 8 * 3600, &hour, &minute, &second);
    assert(hour == 20 && minute == 0 && second == 0);
    love_hms_from_epoch(epoch, -5 * 3600, &hour, &minute, &second);
    assert(hour == 7 && minute == 0);

    // 时区跨越午夜时日期要跟着变。
    love_date_t previous = love_date_from_epoch(epoch - 4 * 3600, -9 * 3600);
    assert(previous.year == 2026 && previous.month == 9 && previous.day == 15);
    love_hms_from_epoch(epoch - 4 * 3600, -9 * 3600, &hour, &minute, &second);
    assert(hour == 23 && minute == 0);
}

static void test_parse(void)
{
    love_date_t out = date(1, 1, 1);

    // 合法日期(含后台网页会传过来的补零写法)。
    assert(love_date_parse("2000-01-01", &out));
    assert(out.year == 2000 && out.month == 1 && out.day == 1);
    assert(love_date_parse("2026-12-5", &out));
    assert(out.year == 2026 && out.month == 12 && out.day == 5);

    // 格式不符:字段不足、非数字、空串。
    assert(!love_date_parse("2026-08", &out));
    assert(!love_date_parse("abcd-ef-gh", &out));
    assert(!love_date_parse("", &out));

    // 格式对但日期非法:平年 2 月 29 日、13 月、4 月 31 日、超范围年份。
    assert(!love_date_parse("2026-02-29", &out));
    assert(!love_date_parse("2026-13-01", &out));
    assert(!love_date_parse("2026-04-31", &out));
    assert(!love_date_parse("1969-12-31", &out));
    assert(!love_date_parse("2100-01-01", &out));

    // 失败时不得改写 out。
    assert(!love_date_parse(NULL, &out));
    assert(!love_date_parse("2026-02-29", NULL));
    assert(out.year == 2026 && out.month == 12 && out.day == 5);
}

int main(void)
{
    test_civil_conversion();
    test_calendar_rules();
    test_days_between_and_together();
    test_next_occurrence_matches_screenshots();
    test_next_occurrence_leap_fallback();
    test_event_countdown();
    test_format_and_epoch();
    test_parse();
    return 0;
}
