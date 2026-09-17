// main/love_config.c —— 配置结构与纯逻辑。见 love_config.h 的说明。
#include "love_config.h"

#include <string.h>

const uint16_t LOVE_BLANK_OFF_SECONDS[LOVE_BLANK_OFF_COUNT] = { 15, 30, 60, 180, 0 };

bool love_blank_off_valid(uint16_t seconds)
{
    for (size_t i = 0; i < LOVE_BLANK_OFF_COUNT; i++) {
        if (LOVE_BLANK_OFF_SECONDS[i] == seconds) return true;
    }
    return false;
}

void love_utf8_copy(char *dst, size_t size, const char *src)
{
    if (!dst || size == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t len = strlen(src);
    if (len >= size) {
        // 截断到缓冲区能装下的长度,但不留下半个多字节字符:
        // UTF-8 续字节的高两位是 10,一直退到某个字符的首字节为止。
        len = size - 1;
        while (len > 0 && ((uint8_t)src[len] & 0xC0) == 0x80) len--;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

void love_config_sanitize(love_config_t *cfg)
{
    if (!cfg) return;

    if (!love_date_valid(cfg->start)) {
        cfg->start = (love_date_t){ 2000, 1, 1 };
    }
    for (size_t i = 0; i < LOVE_PERSON_MAX; i++) {
        cfg->people[i].name[LOVE_NAME_MAX - 1] = '\0';
        if (cfg->people[i].name[0] == '\0') {
            love_utf8_copy(cfg->people[i].name, sizeof(cfg->people[i].name),
                           i == 0 ? "我" : "TA");
        }
        if (cfg->people[i].icon >= LOVE_ICON_TOTAL) cfg->people[i].icon = 0;
    }
    // 熄屏秒数只接受已知档位,别的一律回到默认档。
    if (!love_blank_off_valid(cfg->blank_off_seconds)) {
        cfg->blank_off_seconds = LOVE_BLANK_OFF_DEFAULT;
    }
    if (cfg->ble_enabled > 1) cfg->ble_enabled = 0;

    if (cfg->event_count > LOVE_EVENT_MAX) cfg->event_count = LOVE_EVENT_MAX;

    // 尾部残留:event_count 之外的槽位也要清零。它们会被整条记录落盘,
    // 而后台是按请求顺序整张表重建的 —— 留着旧数据只会让"删掉的事件"
    // 在文件里继续躺着,直到下一次保存。
    memset(&cfg->events[cfg->event_count], 0,
           sizeof(cfg->events) - (size_t)cfg->event_count * sizeof(cfg->events[0]));

    for (size_t i = 0; i < cfg->event_count; i++) {
        love_event_t *event = &cfg->events[i];
        event->name[LOVE_NAME_MAX - 1] = '\0';
        if (event->name[0] == '\0') {
            love_utf8_copy(event->name, sizeof(event->name), "纪念日");
        }
        event->category[LOVE_CATEGORY_MAX - 1] = '\0';
        if (event->icon >= LOVE_ICON_TOTAL) event->icon = 0;
        if (event->kind > LOVE_EVENT_LUNAR) event->kind = LOVE_EVENT_YEARLY;
        // 展示方式只认两个值;坏的按出厂默认(列表)。
        if (event->view_mode != LOVE_EVENT_VIEW_LIST &&
            event->view_mode != LOVE_EVENT_VIEW_PAGE) {
            event->view_mode = LOVE_EVENT_VIEW_LIST;
        }
        if (event->kind == LOVE_EVENT_LUNAR) {
            // 农历事件:month/day 是农历月日,day = 0 表示月末(除夕)。
            // 这里不能按公历校验(date_valid 会要求具体年月日),单独收敛。
            if (event->date.month < 1 || event->date.month > 12) event->date.month = 1;
            if (event->date.day < 0 || event->date.day > 30) event->date.day = 1;
            continue;   // 跳过下面的公历日期校验
        }
        if (!love_date_valid(event->date)) {
            event->date = (love_date_t){ cfg->start.year, 1, 1 };
        }
    }
}

// v2 记录 -> v4:老记录没有分类、没有展示方式与蓝牙开关。分类留空("无分类"),
// 展示方式按"一个事件占一屏"的老行为填单页(升级后看到的屏幕不变),
// 蓝牙开关保持调用方铺好的默认值。
static void migrate_v2(const love_config_v2_t *old, love_config_t *out)
{
    out->start = old->start;
    out->blank_off_seconds = old->blank_off_seconds;

    for (size_t i = 0; i < LOVE_PERSON_MAX; i++) {
        love_utf8_copy(out->people[i].name, sizeof(out->people[i].name),
                       old->people[i].name);
        out->people[i].icon = old->people[i].icon;
    }

    uint8_t count = old->event_count;
    if (count > LOVE_EVENT_MAX) count = LOVE_EVENT_MAX;
    out->event_count = count;

    for (size_t i = 0; i < count; i++) {
        love_utf8_copy(out->events[i].name, sizeof(out->events[i].name),
                       old->events[i].name);
        out->events[i].icon = old->events[i].icon;
        out->events[i].kind = old->events[i].kind;
        out->events[i].date = old->events[i].date;
        out->events[i].category[0] = '\0';
        // v2 时代没有展示方式这个概念,设备就是"一个事件一屏"。
        out->events[i].view_mode = LOVE_EVENT_VIEW_PAGE;
    }
}

// v3 记录 -> v4:字段基本一一对应,只有两处不同:
//   1. v3 是**全局**一个展示模式,v4 变成每条事件自己带 —— 把全局值摊到每条上,
//      升级后屏幕上看到的东西不变;
//   2. 事件上限从 8 提到 LOVE_EVENT_MAX,超出部分只能丢(现实中不会有人有)。
static void migrate_v3(const love_config_v3_t *old, love_config_t *out)
{
    out->start = old->start;
    out->blank_off_seconds = old->blank_off_seconds;
    out->ble_enabled = (old->ble_enabled > 1) ? 0 : old->ble_enabled;

    for (size_t i = 0; i < LOVE_PERSON_MAX; i++) {
        love_utf8_copy(out->people[i].name, sizeof(out->people[i].name),
                       old->people[i].name);
        out->people[i].icon = old->people[i].icon;
    }

    uint8_t count = old->event_count;
    if (count > 8) count = 8;                 // v3 的记录里最多就 8 条
    if (count > LOVE_EVENT_MAX) count = LOVE_EVENT_MAX;
    out->event_count = count;

    // v3 的 display_mode 里 0 = 列表、1 = 单页(写死字面量:那个常量已经删了,
    // 这里是历史格式的一部分,不能跟着新代码走)。
    const uint8_t view = (old->display_mode == 0u) ? LOVE_EVENT_VIEW_LIST
                                                    : LOVE_EVENT_VIEW_PAGE;
    for (size_t i = 0; i < count; i++) {
        love_utf8_copy(out->events[i].name, sizeof(out->events[i].name),
                       old->events[i].name);
        out->events[i].icon = old->events[i].icon;
        out->events[i].kind = old->events[i].kind;
        out->events[i].date = old->events[i].date;
        love_utf8_copy(out->events[i].category, sizeof(out->events[i].category),
                       old->events[i].category);
        out->events[i].view_mode = view;
    }
}

bool love_config_from_record(const void *blob, size_t size, love_config_t *out)
{
    if (!blob || !out || size < sizeof(uint32_t)) return false;

    uint32_t version = 0;
    memcpy(&version, blob, sizeof(version));   // 版本号是首字段,任何版本都能先读出来

    // memcpy 进局部结构再用,不直接往 blob 上套结构体指针:nvs 给的缓冲不保证对齐。
    if (version == LOVE_CONFIG_VERSION && size == sizeof(love_config_record_t)) {
        love_config_record_t record;
        memcpy(&record, blob, sizeof(record));
        *out = record.config;
        return true;
    }

    if (version == 3u && size == sizeof(love_config_v3_record_t)) {
        love_config_v3_record_t record;
        memcpy(&record, blob, sizeof(record));
        migrate_v3(&record.config, out);
        return true;
    }

    if (version == 2u && size == sizeof(love_config_v2_record_t)) {
        love_config_v2_record_t record;
        memcpy(&record, blob, sizeof(record));
        migrate_v2(&record.config, out);
        return true;
    }

    return false;
}
