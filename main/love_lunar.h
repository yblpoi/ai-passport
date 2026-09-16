// main/love_lunar.h —— 农历换算(纯逻辑层)。
//
// 与 love_date.c 一样不依赖 ESP-IDF 与 LVGL,可被 host tests 直接覆盖
// (tests/test_love_lunar.c)。数据表由 tools/gen_lunar_table.py 生成,
// 生成时已用公开来源的春节日期逐年反查。
#pragma once

#include "love_date.h"

#include <stdbool.h>
#include <stddef.h>

// 闰月不参与选择:春节永远是"正月初一",不会是"闰正月初一"。
// 所以事件里的农历月日一律指普通月;闰月只在天数累加时计入。
//
// 农历日传 0 表示"该月最后一天",用于除夕(腊月最后一天)。

// 农历 (year, month, day) -> 公历。year 是农历年号(如 2026 年正月初一)。
// 超出数据表覆盖范围、或月日不合法时返回 false,不猜。
bool love_lunar_to_solar(int lunar_year, int lunar_month, int lunar_day, love_date_t *out);

// 某个"每年重复"的农历月日(如 八月十五)在 today 当天或之后的下一次公历日期。
// 今天当天算今天。超出数据表范围返回 false。
bool love_lunar_next_occurrence(love_date_t today, int lunar_month, int lunar_day,
                                love_date_t *out);

// 该农历月的天数;月份或年份不合法返回 0。
int love_lunar_month_days(int lunar_year, int lunar_month);

// 当年闰月号(1..12),无闰月返回 0。
int love_lunar_leap_month(int lunar_year);

// 把农历月日写成"八月十五""腊月廿九"这类可读文本(UTF-8)。
// 传 day = 0(月末)时写成"腊月最后一天"。buf 建议至少 32 字节。
void love_lunar_format(int lunar_month, int lunar_day, char *buf, size_t size);

// 农历月日的按键编辑:field 0 = 月(1..12 环绕),field 1 = 日(0..30 环绕,0 表示月末)。
// 与公历的 love_date_step 分开,因为农历月长不是公历的 28/30/31。
void love_lunar_step(int *lunar_month, int *lunar_day, int field, int delta);
