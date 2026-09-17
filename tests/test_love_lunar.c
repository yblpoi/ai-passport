// tests/test_love_lunar.c —— 农历换算的 host 测试。
//
// 参考值全部来自公开来源，并与 tools/gen_lunar_table.py 的反查一致：
//   春节(正月初一)：2018-02-16 / 2020-01-25 / 2024-02-10 / 2025-01-29 /
//                   2026-02-17 / 2027-02-06 / 2028-01-26 / 2030-02-03 /
//                   2033-01-31 / 2035-02-08
//   其他：2026 除夕 2026-02-16、元宵 2026-03-03、端午 2026-06-19、
//         七夕 2026-08-19、中秋 2026-09-25、重阳 2026-10-18
// 表一旦算错，这些断言会立刻失败 —— 农历算错比不做更糟，所以要钉死。
#include "love_date.h"
#include "love_lunar.h"

#include <assert.h>
#include <string.h>

static love_date_t date(int y, int m, int d)
{
    love_date_t out = { (int16_t)y, (int8_t)m, (int8_t)d };
    return out;
}

static void expect_solar(int lunar_year, int month, int day, int y, int m, int d)
{
    love_date_t out = { 0, 0, 0 };
    assert(love_lunar_to_solar(lunar_year, month, day, &out));
    assert(out.year == y && out.month == m && out.day == d);
}

static void test_spring_festival(void)
{
    // 正月初一 = 春节
    expect_solar(2018, 1, 1, 2018, 2, 16);
    expect_solar(2020, 1, 1, 2020, 1, 25);
    expect_solar(2024, 1, 1, 2024, 2, 10);
    expect_solar(2025, 1, 1, 2025, 1, 29);
    expect_solar(2026, 1, 1, 2026, 2, 17);
    expect_solar(2027, 1, 1, 2027, 2, 6);
    expect_solar(2028, 1, 1, 2028, 1, 26);
    expect_solar(2030, 1, 1, 2030, 2, 3);
    expect_solar(2033, 1, 1, 2033, 1, 31);
    expect_solar(2035, 1, 1, 2035, 2, 8);
}

static void test_other_festivals(void)
{
    expect_solar(2026, 1, 15, 2026, 3, 3);    // 元宵
    expect_solar(2026, 5, 5, 2026, 6, 19);    // 端午
    expect_solar(2026, 7, 7, 2026, 8, 19);    // 七夕
    expect_solar(2026, 8, 15, 2026, 9, 25);   // 中秋
    expect_solar(2026, 9, 9, 2026, 10, 18);   // 重阳

    // 除夕：腊月最后一天(day = 0)。2025 农历年的腊月是 29 天，落在 2026-02-16，
    // 正好是 2026 年春节前一天。
    expect_solar(2025, 12, 0, 2026, 2, 16);
}

static void test_leap_month_years(void)
{
    // 闰月只影响天数累加，不影响"普通月第几天"的选取。
    // 2025 是闰六月年，中秋(八月十五)必须仍然落在大后方的正确日期。
    assert(love_lunar_leap_month(2020) == 4);
    assert(love_lunar_leap_month(2023) == 2);
    assert(love_lunar_leap_month(2025) == 6);
    assert(love_lunar_leap_month(2026) == 0);
    assert(love_lunar_leap_month(2028) == 5);
    expect_solar(2025, 8, 15, 2025, 10, 6);   // 2025 中秋

    // 闰月本身不参与选择：闰六月的"六月"取普通六月，日期必须早于闰六月。
    love_date_t plain = { 0, 0, 0 };
    assert(love_lunar_to_solar(2025, 6, 1, &plain));
    assert(plain.month == 6 && plain.day == 25);   // 2025 六月初一 = 2025-06-25
}

static void test_next_occurrence(void)
{
    love_date_t out = { 0, 0, 0 };

    // 元旦之后查春节 -> 当年春节
    assert(love_lunar_next_occurrence(date(2026, 1, 1), 1, 1, &out));
    assert(out.year == 2026 && out.month == 2 && out.day == 17);

    // 当天算当天
    assert(love_lunar_next_occurrence(date(2026, 2, 17), 1, 1, &out));
    assert(out.year == 2026 && out.month == 2 && out.day == 17);

    // 春节次日查 -> 下一年春节
    assert(love_lunar_next_occurrence(date(2026, 2, 18), 1, 1, &out));
    assert(out.year == 2027 && out.month == 2 && out.day == 6);

    // 中秋同理：2026 中秋 09-25，过后查 2027
    assert(love_lunar_next_occurrence(date(2026, 9, 26), 8, 15, &out));
    assert(out.year == 2027 && out.month == 9 && out.day == 15);

    // 腊月(day = 0 月末)也能查：2026 年里的除夕是 2027 农历年腊月末
    assert(love_lunar_next_occurrence(date(2026, 3, 1), 12, 0, &out));
    assert(out.year == 2027 && out.month == 2 && out.day == 5);   // 2027 春节前一天
}

static void test_out_of_range(void)
{
    love_date_t out = { 0, 0, 0 };

    // 表覆盖 2018..2050：范围外必须明确失败，不能猜一个日期出来
    assert(!love_lunar_to_solar(2017, 1, 1, &out));
    assert(!love_lunar_to_solar(2051, 1, 1, &out));
    assert(!love_lunar_next_occurrence(date(2060, 1, 1), 1, 1, &out));
    // day = 0 表示"该月最后一天"，是合法输入(除夕就靠它)
    assert(love_lunar_to_solar(2026, 5, 0, &out));
    assert(!love_lunar_to_solar(2026, 5, 31, &out));  // 农历月最多 30 天
    assert(!love_lunar_to_solar(2026, 13, 1, &out));  // 没有十三月

    assert(love_lunar_month_days(2026, 5) == 29 || love_lunar_month_days(2026, 5) == 30);
    assert(love_lunar_month_days(2026, 13) == 0);
    assert(love_lunar_month_days(2017, 1) == 0);
}

static void test_month_days_sum(void)
{
    // 每个普通月只能是 29 或 30 天；闰月号必须在 1..12 内。
    // 这里不去猜闰月天数（表里单独存），只检查确实存进去的普通月的合法性。
    for (int y = 2018; y <= 2050; y++) {
        for (int m = 1; m <= 12; m++) {
            const int days = love_lunar_month_days(y, m);
            assert(days == 29 || days == 30);
        }
        const int leap = love_lunar_leap_month(y);
        assert(leap >= 0 && leap <= 12);
    }
}

static void test_format(void)
{
    char buf[32];

    love_lunar_format(8, 15, buf, sizeof(buf));
    assert(strcmp(buf, "八月十五") == 0);

    love_lunar_format(1, 1, buf, sizeof(buf));
    assert(strcmp(buf, "正月初一") == 0);

    love_lunar_format(12, 0, buf, sizeof(buf));
    assert(strcmp(buf, "腊月最后一天") == 0);

    // 21..29 用「二十一..二十九」而不是「廿一..廿九」:像素字库没有「廿」的字形,
    // 用「廿」设备上会画成方框。day==20 本来就写作「二十」,这样也统一。
    love_lunar_format(8, 22, buf, sizeof(buf));
    assert(strcmp(buf, "八月二十二") == 0);

    love_lunar_format(8, 20, buf, sizeof(buf));
    assert(strcmp(buf, "八月二十") == 0);

    love_lunar_format(8, 29, buf, sizeof(buf));
    assert(strcmp(buf, "八月二十九") == 0);

    love_lunar_format(8, 30, buf, sizeof(buf));
    assert(strcmp(buf, "八月三十") == 0);

    love_lunar_format(4, 10, buf, sizeof(buf));
    assert(strcmp(buf, "四月初十") == 0);

    love_lunar_format(0, 5, buf, sizeof(buf));
    assert(strcmp(buf, "农历") == 0);
}

static void test_event_countdown_lunar(void)
{
    love_event_t event = { .name = "春节", .icon = 0, .kind = LOVE_EVENT_LUNAR,
                           .date = { 2026, 1, 1 } };
    love_countdown_t out = love_event_countdown(&event, date(2026, 1, 28));
    assert(out.resolved);
    assert(out.target.year == 2026 && out.target.month == 2 && out.target.day == 17);
    assert(out.days == 20 && out.upcoming);

    // 超出农历表范围的年份必须标成未解析，而不是给个假日期
    love_countdown_t far = love_event_countdown(&event, date(2060, 1, 1));
    assert(!far.resolved);
}

int main(void)
{
    test_spring_festival();
    test_other_festivals();
    test_leap_month_years();
    test_next_occurrence();
    test_out_of_range();
    test_month_days_sum();
    test_format();
    test_event_countdown_lunar();
    return 0;
}
