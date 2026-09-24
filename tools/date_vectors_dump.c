// tools/date_vectors_dump.c —— 用**设备端实现**生成预览测试向量。
//
// 它不是固件的一部分,也不在 tests/ 里:由 tools/gen_date_vectors.py 编译并运行,把
// 结果写成 tests/vectors/date_vectors.json,再由 tests/test_preview_math.mjs 用来核对
// 后台页那份 JS 实现。于是"设备端是唯一事实源"不是口头约定,而是一条可复算的链:
//
//     love_date.c / love_lunar.c  →  本程序  →  date_vectors.json  →  JS 测试
//
// 用法(由 gen_date_vectors.py 调用,不手工跑):
//     cc -std=c11 -Imain tools/date_vectors_dump.c main/love_date.c main/love_lunar.c -o dump
//     ./dump > vectors.json
//
// 输出是 JSON 片段,由 Python 侧统一排版(所以这里不追求好看的缩进)。
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "love_date.h"
#include "love_lunar.h"

/* ---------- 小的 JSON 输出工具 ---------- */

static void json_string(const char *text)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == '"' || *p == '\\') {
            putchar('\\');
            putchar((char)*p);
        } else if (*p < 0x20) {
            printf("\\u%04x", (unsigned)*p);
        } else {
            putchar((char)*p);
        }
    }
    putchar('"');
}

static void json_date_or_null(const love_date_t *date)
{
    if (!date) {
        fputs("null", stdout);
        return;
    }
    char buf[16];
    love_date_format(*date, buf, sizeof(buf));
    json_string(buf);
}

/* ---------- 一、事件倒计时:每年 / 仅一次 / 农历 ---------- */

typedef struct {
    const char *today;
    love_event_kind_t kind;
    int month;          // YEARLY / LUNAR 用
    int day;            // YEARLY / LUNAR 用;0 = 该月最后一天
    const char *date;   // ONCE 用
} event_case_t;

// 覆盖面按"出过事或最容易出事的边界"选:
//   每年重复:今年还没到 / 就在今天 / 今年已经过了 / 2 月 29 落在平年与闰年;
//   仅一次:未来 / 当天 / 过去(过期要显示"已过 N 天");
//   农历:中秋 / 春节 / 除夕(day = 0)/ 数据表两端 / 超出表范围的一年。
static const event_case_t EVENT_CASES[] = {
    { "2026-09-22", LOVE_EVENT_YEARLY,  1,  1, NULL },
    { "2026-09-22", LOVE_EVENT_YEARLY, 12, 25, NULL },
    { "2026-12-25", LOVE_EVENT_YEARLY, 12, 25, NULL },
    { "2027-02-28", LOVE_EVENT_YEARLY,  2, 29, NULL },
    { "2028-02-28", LOVE_EVENT_YEARLY,  2, 29, NULL },
    { "2026-12-31", LOVE_EVENT_YEARLY,  1,  1, NULL },
    { "2026-09-22", LOVE_EVENT_ONCE,    0,  0, "2026-10-01" },
    { "2026-09-22", LOVE_EVENT_ONCE,    0,  0, "2026-09-22" },
    { "2026-09-22", LOVE_EVENT_ONCE,    0,  0, "2026-01-01" },
    { "2026-09-22", LOVE_EVENT_ONCE,    0,  0, "1999-12-31" },
    { "2026-09-22", LOVE_EVENT_LUNAR,   8, 15, NULL },
    { "2026-01-01", LOVE_EVENT_LUNAR,   1,  1, NULL },
    { "2026-02-10", LOVE_EVENT_LUNAR,  12,  0, NULL },
    { "2018-01-01", LOVE_EVENT_LUNAR,   1,  1, NULL },
    { "2050-06-01", LOVE_EVENT_LUNAR,   8, 15, NULL },
    { "2051-01-01", LOVE_EVENT_LUNAR,   1,  1, NULL },
};

static const char *kind_name(love_event_kind_t kind)
{
    switch (kind) {
    case LOVE_EVENT_YEARLY: return "yearly";
    case LOVE_EVENT_ONCE:   return "once";
    case LOVE_EVENT_LUNAR:  return "lunar";
    }
    return "unknown";
}

static void dump_event_case(const event_case_t *c)
{
    love_date_t today = { 0, 0, 0 };
    if (!love_date_parse(c->today, &today)) {
        fprintf(stderr, "日期写错了: %s\n", c->today);
        return;
    }

    love_event_t event = { 0 };
    event.kind = (uint8_t)c->kind;
    if (c->kind == LOVE_EVENT_ONCE) {
        if (!love_date_parse(c->date, &event.date)) {
            fprintf(stderr, "日期写错了: %s\n", c->date);
            return;
        }
    } else {
        // 年份对这两种重复方式没有意义(只按公历/农历的月日折算),写今天的年份让 JSON 可读。
        event.date = (love_date_t){ today.year, (int8_t)c->month, (int8_t)c->day };
    }

    const love_countdown_t countdown = love_event_countdown(&event, today);

    fputs("    { \"today\": ", stdout);
    json_string(c->today);
    fputs(", \"kind\": ", stdout);
    json_string(kind_name(c->kind));
    fputs(", \"month\": ", stdout);
    if (c->kind == LOVE_EVENT_ONCE) fputs("null", stdout); else printf("%d", c->month);
    fputs(", \"day\": ", stdout);
    if (c->kind == LOVE_EVENT_ONCE) fputs("null", stdout); else printf("%d", c->day);
    fputs(", \"date\": ", stdout);
    if (c->kind == LOVE_EVENT_ONCE) json_string(c->date); else fputs("null", stdout);
    fputs(", \"target\": ", stdout);
    // 算不出来(农历超出数据表)时不给日期:设备那边同样只显示"农历超出范围"。
    json_date_or_null(countdown.resolved ? &countdown.target : NULL);
    fputs(", \"days\": ", stdout);
    if (countdown.resolved) printf("%d", (int)countdown.days); else fputs("null", stdout);
    fputs(", \"resolved\": ", stdout);
    fputs(countdown.resolved ? "true" : "false", stdout);
    fputs(" }", stdout);
}

/* ---------- 二、"在一起 N 天" ---------- */

static const struct {
    const char *start;
    const char *today;
} TOGETHER_CASES[] = {
    { "2000-01-01", "2000-01-01" },   // 起始日当天 = 1
    { "2000-01-01", "2000-01-02" },
    { "2026-09-22", "2026-09-22" },
    { "2026-09-22", "2026-09-21" },   // 今天早于起始日 = 0
    { "2024-02-29", "2025-02-28" },   // 跨闰年
    { "2016-02-29", "2028-02-29" },
    { "2000-01-01", "2099-12-31" },
};

static void dump_together_case(const char *start_text, const char *today_text)
{
    love_date_t start = { 0, 0, 0 };
    love_date_t today = { 0, 0, 0 };
    if (!love_date_parse(start_text, &start) || !love_date_parse(today_text, &today)) {
        fprintf(stderr, "日期写错了: %s / %s\n", start_text, today_text);
        return;
    }
    fputs("    { \"start\": ", stdout);
    json_string(start_text);
    fputs(", \"today\": ", stdout);
    json_string(today_text);
    printf(", \"days\": %d }", (int)love_days_together(start, today));
}

/* ---------- 三、农历换算与中文写法 ---------- */

static const struct {
    int year;
    int month;
    int day;
} LUNAR_SOLAR_CASES[] = {
    { 2026,  1,  1 },   // 表覆盖范围内的春节
    { 2026,  8, 15 },   // 中秋
    { 2026, 12,  0 },   // 除夕:day = 0 = 月末
    { 2018,  1,  1 },   // 数据表第一年
    { 2050,  1,  1 },   // 数据表最后一年
    { 2051,  1,  1 },   // 超出范围 -> null
    { 2026, 13,  1 },   // 月份不合法 -> null
    { 2026,  1, 31 },   // 该月没有第 31 天 -> null
};

static const struct {
    int month;
    int day;
} LUNAR_NAME_CASES[] = {
    {  1,  1 }, {  1, 10 }, {  1, 11 }, {  1, 20 }, {  1, 21 }, {  1, 29 },
    {  8, 15 }, { 12,  0 }, { 12, 30 }, {  8, 31 }, {  0,  8 }, { 13,  8 },
};

int main(void)
{
    fputs("{\n", stdout);

    fputs("  \"events\": [\n", stdout);
    for (size_t i = 0; i < sizeof(EVENT_CASES) / sizeof(EVENT_CASES[0]); i++) {
        if (i) fputs(",\n", stdout);
        dump_event_case(&EVENT_CASES[i]);
    }
    fputs("\n  ],\n", stdout);

    fputs("  \"days_together\": [\n", stdout);
    for (size_t i = 0; i < sizeof(TOGETHER_CASES) / sizeof(TOGETHER_CASES[0]); i++) {
        if (i) fputs(",\n", stdout);
        dump_together_case(TOGETHER_CASES[i].start, TOGETHER_CASES[i].today);
    }
    fputs("\n  ],\n", stdout);

    fputs("  \"lunar_solar\": [\n", stdout);
    for (size_t i = 0; i < sizeof(LUNAR_SOLAR_CASES) / sizeof(LUNAR_SOLAR_CASES[0]); i++) {
        const int year = LUNAR_SOLAR_CASES[i].year;
        const int month = LUNAR_SOLAR_CASES[i].month;
        const int day = LUNAR_SOLAR_CASES[i].day;
        love_date_t solar = { 0, 0, 0 };
        const bool ok = love_lunar_to_solar(year, month, day, &solar);
        if (i) fputs(",\n", stdout);
        printf("    { \"year\": %d, \"month\": %d, \"day\": %d, \"solar\": ", year, month, day);
        json_date_or_null(ok ? &solar : NULL);
        fputs(" }", stdout);
    }
    fputs("\n  ],\n", stdout);

    fputs("  \"lunar_names\": [\n", stdout);
    for (size_t i = 0; i < sizeof(LUNAR_NAME_CASES) / sizeof(LUNAR_NAME_CASES[0]); i++) {
        char name[32];
        love_lunar_format(LUNAR_NAME_CASES[i].month, LUNAR_NAME_CASES[i].day, name, sizeof(name));
        if (i) fputs(",\n", stdout);
        printf("    { \"month\": %d, \"day\": %d, \"text\": ",
               LUNAR_NAME_CASES[i].month, LUNAR_NAME_CASES[i].day);
        json_string(name);
        fputs(" }", stdout);
    }
    fputs("\n  ]\n", stdout);

    fputs("}\n", stdout);
    return 0;
}
