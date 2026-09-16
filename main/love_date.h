// main/love_date.h —— 恋爱倒计时的纯逻辑层:公历换算、天数差、年度事件折算。
//
// 本文件不依赖 ESP-IDF 与 LVGL,可被 host tests 直接编译覆盖
// (tests/test_love_date.c)。界面、NVS、网络一律在应用层,不要往这里塞。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// UTF-8 字节上限(含结尾 NUL)。中文字符约占 3 字节,24 字节约 8 个汉字。
#define LOVE_NAME_MAX 25
// 设备端最多保存的事件条目数(与 lovestore 的 NVS 记录一一对应)。
#define LOVE_EVENT_MAX 8
// 主屏底部的两个人。
#define LOVE_PERSON_MAX 2
// 内置像素图标数量(love_icons.c)。
#define LOVE_ICON_MAX 16
// 用户上传的自定义头像槽位数:图标号 LOVE_ICON_MAX .. LOVE_ICON_MAX+LOVE_AVATAR_MAX-1。
#define LOVE_AVATAR_MAX 4
// 合法的图标号上界(不含)。内置图标 + 自定义头像。
#define LOVE_ICON_TOTAL (LOVE_ICON_MAX + LOVE_AVATAR_MAX)

typedef struct {
    int16_t year;   // 1970..2099
    int8_t month;   // 1..12
    int8_t day;     // 1..love_days_in_month(year, month)
} love_date_t;

// 事件重复方式:一次性(生日按年、节日按年)、仅一次(如某个具体纪念日)、
// 或农历节日(按农历月日每年折算)。
typedef enum {
    LOVE_EVENT_YEARLY = 0,   // 每年重复(生日、节日):自动折算到下一次发生日
    LOVE_EVENT_ONCE = 1,     // 只算这一个日期:过期后显示“已过 N 天”
    LOVE_EVENT_LUNAR = 2,    // 农历每年重复:date.month/day 为农历月日,day=0 表示月末(除夕)
} love_event_kind_t;

typedef struct {
    char name[LOVE_NAME_MAX];   // UTF-8
    uint8_t icon;               // 0..LOVE_ICON_TOTAL-1,见 love_pixel_art.h
    uint8_t kind;               // love_event_kind_t
    love_date_t date;           // YEARLY/LUNAR 只用 month/day,ONCE 用完整日期
                                // (LUNAR 时 month/day 是农历)
} love_event_t;

// 日期编辑界面里被选中的字段。
enum {
    LOVE_FIELD_YEAR = 0,
    LOVE_FIELD_MONTH,
    LOVE_FIELD_DAY,
    LOVE_FIELD_COUNT,
};

// 倒计时结果。
typedef struct {
    int32_t days;        // >=0 表示“N 天后”;<0 表示已过 |days| 天
    bool upcoming;       // days >= 0
    bool resolved;       // false = 农历年份超出数据表范围,target/days 不可信
    love_date_t target;  // 实际倒数目标日(YEARLY/LUNAR 已折算到下一次发生日)
} love_countdown_t;

bool love_is_leap(int year);
int love_days_in_month(int year, int month);
bool love_date_valid(love_date_t d);

// 公历 <-> 纪元天数(1970-01-01 为 0)。
int64_t love_days_from_civil(love_date_t d);
love_date_t love_civil_from_days(int64_t days);

// to - from,单位天。
int32_t love_days_between(love_date_t from, love_date_t to);

// “在一起 N 天”:含起始日当天,起始日当天为 1。若 today 早于 start 返回 0。
int32_t love_days_together(love_date_t start, love_date_t today);

// 年度事件的下一次发生日:今天当天算“今天”,今年已过则取明年。
// 目标日为 2 月 29 日而该年不是闰年时,折算到 2 月 28 日。
love_date_t love_next_occurrence(love_date_t today, int month, int day);

// 事件倒计时。YEARLY 用 month/day 折算;ONCE 用完整日期;LUNAR 按农历月日折算。
love_countdown_t love_event_countdown(const love_event_t *event, love_date_t today);

// 编辑日期:field 见 LOVE_FIELD_*,delta 通常为 ±1。
// 月按 1..12 循环、日按当月天数循环、年夹在 1970..2099。
love_date_t love_date_step(love_date_t d, int field, int delta);

// 写 "YYYY-MM-DD" 到 buf,需要至少 11 字节;不足时写入空串。
void love_date_format(love_date_t d, char *buf, size_t size);

// UTC 秒 -> 当地日期/时间(东八区传 8*3600)。
love_date_t love_date_from_epoch(uint64_t epoch_seconds, int32_t tz_offset_seconds);
void love_hms_from_epoch(uint64_t epoch_seconds, int32_t tz_offset_seconds,
                         int *hour, int *minute, int *second);
