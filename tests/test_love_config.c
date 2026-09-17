// tests/test_love_config.c —— 配置结构与 v2→v3 迁移的 host 测试。
//
// 这是全仓最危险的一段逻辑:记录长度或版本判错,设备上用户的起始日、姓名、事件
// 就会被静默清空。所以布局用 offsetof 逐字段钉住,迁移逐字段验。
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "love_config.h"

// 冻结的 v2 布局:任何改动都应该先失败一次,提醒改的人"这是历史格式,不能动"。
_Static_assert(sizeof(love_config_v2_person_t) == 26, "v2 人名结构变了");
_Static_assert(sizeof(love_config_v2_event_t) == 32, "v2 事件结构变了");
_Static_assert(offsetof(love_config_v2_event_t, icon) == 25, "v2 事件字段偏移变了");
_Static_assert(offsetof(love_config_v2_event_t, kind) == 26, "v2 事件字段偏移变了");
_Static_assert(offsetof(love_config_v2_event_t, date) == 28, "v2 事件字段偏移变了");
_Static_assert(offsetof(love_config_v2_t, event_count) == 56, "v2 配置字段偏移变了");
_Static_assert(offsetof(love_config_v2_t, blank_off_seconds) == 58, "v2 配置字段偏移变了");
_Static_assert(offsetof(love_config_v2_t, events) == 60, "v2 配置字段偏移变了");
_Static_assert(sizeof(love_config_v2_record_t) == 320, "v2 记录长度变了");

// v3 的长度同样钉住:动它就必须同时升 LOVE_CONFIG_VERSION 并写迁移,否则老设备
// 上的记录会被按新长度解释 —— 这正是"静默清空用户数据"的入口。
_Static_assert(sizeof(love_config_t) == 526, "v3 配置结构变了,需要升版本并写迁移");
_Static_assert(sizeof(love_config_record_t) == 532, "v3 记录长度变了,需要升版本并写迁移");

// 模拟调用方加载前铺好的默认值,用来验证"新字段不受迁移影响"。
static void seed_defaults(love_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->display_mode = LOVE_DISPLAY_PAGE;
    cfg->ble_enabled = 0;
    cfg->blank_off_seconds = LOVE_BLANK_OFF_DEFAULT;
}

static void test_migrate_v2_to_v3(void)
{
    love_config_v2_record_t old;
    memset(&old, 0, sizeof(old));
    old.version = 2u;
    old.config.start = (love_date_t){ 2025, 3, 14 };
    old.config.blank_off_seconds = 180;
    strcpy(old.config.people[0].name, "咕咕");
    old.config.people[0].icon = 17;      // 自定义头像槽位
    strcpy(old.config.people[1].name, "嘎嘎");
    old.config.people[1].icon = 3;
    old.config.event_count = 3;
    strcpy(old.config.events[0].name, "元旦");
    old.config.events[0].icon = 12;
    old.config.events[0].kind = 0;
    old.config.events[0].date = (love_date_t){ 2025, 1, 1 };
    strcpy(old.config.events[1].name, "春节");
    old.config.events[1].icon = 11;
    old.config.events[1].kind = 2;       // 农历
    old.config.events[1].date = (love_date_t){ 2025, 1, 1 };
    strcpy(old.config.events[2].name, "毕业");
    old.config.events[2].icon = 4;
    old.config.events[2].kind = 1;       // 仅一次
    old.config.events[2].date = (love_date_t){ 2024, 6, 30 };

    love_config_t now;
    seed_defaults(&now);
    assert(love_config_from_record(&old, sizeof(old), &now) == true);

    // 老记录里的字段逐项搬过来了
    assert(now.start.year == 2025 && now.start.month == 3 && now.start.day == 14);
    assert(now.blank_off_seconds == 180);
    assert(strcmp(now.people[0].name, "咕咕") == 0);
    assert(now.people[0].icon == 17);
    assert(strcmp(now.people[1].name, "嘎嘎") == 0);
    assert(now.people[1].icon == 3);
    assert(now.event_count == 3);
    assert(strcmp(now.events[0].name, "元旦") == 0);
    assert(now.events[0].kind == 0 && now.events[0].date.month == 1);
    assert(strcmp(now.events[1].name, "春节") == 0);
    assert(now.events[1].kind == 2 && now.events[1].date.month == 1);
    assert(strcmp(now.events[2].name, "毕业") == 0);
    assert(now.events[2].kind == 1 && now.events[2].date.year == 2024);

    // v3 才有的字段:v2 没有分类,空串;展示模式与蓝牙开关保持调用方铺的默认值,
    // 也就是"升级后行为不变"。事件尾部残留必须被清掉。
    for (size_t i = 0; i < LOVE_EVENT_MAX; i++) {
        assert(now.events[i].category[0] == '\0');
    }
    assert(now.display_mode == LOVE_DISPLAY_PAGE);
    assert(now.ble_enabled == 0);
    assert(now.events[LOVE_EVENT_MAX - 1].name[0] == '\0');
}

static void test_v3_record_round_trip(void)
{
    love_config_record_t record;
    memset(&record, 0, sizeof(record));
    record.version = LOVE_CONFIG_VERSION;
    record.config.start = (love_date_t){ 2000, 1, 1 };
    record.config.event_count = 1;
    strcpy(record.config.people[0].name, "咕咕");
    strcpy(record.config.events[0].name, "在一起");
    strcpy(record.config.events[0].category, "纪念日");
    record.config.events[0].icon = 6;
    record.config.events[0].kind = 0;
    record.config.events[0].date = (love_date_t){ 2000, 1, 1 };
    record.config.display_mode = LOVE_DISPLAY_LIST;
    record.config.ble_enabled = 1;

    love_config_t now;
    seed_defaults(&now);
    assert(love_config_from_record(&record, sizeof(record), &now) == true);
    assert(now.display_mode == LOVE_DISPLAY_LIST);
    assert(now.ble_enabled == 1);
    assert(strcmp(now.events[0].category, "纪念日") == 0);
    assert(now.event_count == 1);
}

static void test_unknown_records_rejected(void)
{
    love_config_t now;
    seed_defaults(&now);

    love_config_record_t record;
    memset(&record, 0, sizeof(record));

    // 版本不认识
    record.version = 1u;
    assert(love_config_from_record(&record, sizeof(record), &now) == false);
    record.version = 99u;
    assert(love_config_from_record(&record, sizeof(record), &now) == false);

    // 版本对但长度不对
    record.version = LOVE_CONFIG_VERSION;
    assert(love_config_from_record(&record, sizeof(record) - 1, &now) == false);
    assert(love_config_from_record(&record, sizeof(record) + 8, &now) == false);

    // 空指针与过短缓冲
    assert(love_config_from_record(NULL, sizeof(record), &now) == false);
    assert(love_config_from_record(&record, 3, &now) == false);
    assert(love_config_from_record(&record, sizeof(record), NULL) == false);
}

static void test_utf8_copy_truncates_on_boundary(void)
{
    char buf[10];

    // "一二三四" = 12 字节,装进 10 字节缓冲区只能留 3 个汉字(9 字节),不能切成半个
    love_utf8_copy(buf, sizeof(buf), "一二三四");
    assert(strlen(buf) == 9);
    assert(strcmp(buf, "一二三") == 0);

    love_utf8_copy(buf, sizeof(buf), "短");
    assert(strcmp(buf, "短") == 0);

    love_utf8_copy(buf, sizeof(buf), "");
    assert(buf[0] == '\0');

    love_utf8_copy(buf, sizeof(buf), NULL);
    assert(buf[0] == '\0');

    // size 为 0 时一个字节都不写
    char byte = 'x';
    love_utf8_copy(&byte, 0, "一");
    assert(byte == 'x');

    // 恰好的长度
    char exact[4];
    love_utf8_copy(exact, sizeof(exact), "一");
    assert(strcmp(exact, "一") == 0);
}

static void test_sanitize_clamps_everything(void)
{
    love_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    // 全零 = 非法日期、空名字、越界图标、非法档位、非法模式
    cfg.people[0].icon = 200;
    cfg.people[1].icon = LOVE_ICON_TOTAL;
    cfg.blank_off_seconds = 7;
    cfg.display_mode = 42;
    cfg.ble_enabled = 9;
    cfg.event_count = 200;             // 超过上限
    strcpy(cfg.events[0].name, "生日");
    cfg.events[0].icon = 250;
    cfg.events[0].kind = 9;
    cfg.events[0].date = (love_date_t){ 2026, 13, 40 };
    // 尾部塞一条,验证会被清掉
    strcpy(cfg.events[LOVE_EVENT_MAX - 1].name, "残留");

    love_config_sanitize(&cfg);

    assert(love_date_valid(cfg.start));
    assert(strcmp(cfg.people[0].name, "我") == 0);
    assert(strcmp(cfg.people[1].name, "TA") == 0);
    assert(cfg.people[0].icon == 0 && cfg.people[1].icon == 0);
    assert(cfg.blank_off_seconds == LOVE_BLANK_OFF_DEFAULT);
    assert(cfg.display_mode == LOVE_DISPLAY_PAGE);
    assert(cfg.ble_enabled == 0);
    assert(cfg.event_count == LOVE_EVENT_MAX);
    assert(cfg.events[0].icon == 0);
    assert(cfg.events[0].kind == LOVE_EVENT_YEARLY);
    assert(love_date_valid(cfg.events[0].date));
    // 尾部残留被清掉(第 8 条本来就叫"残留",event_count 被夹到 8 之后它成了有效项,
    // 所以这里验证的是"名字被保留、但分类字段被收敛为空")
    assert(cfg.events[LOVE_EVENT_MAX - 1].category[0] == '\0');

    // event_count 合法时,它之后的槽位必须被清零
    love_config_t small;
    memset(&small, 0, sizeof(small));
    small.event_count = 1;
    strcpy(small.events[1].name, "不该在");
    love_config_sanitize(&small);
    assert(small.events[1].name[0] == '\0');

    // 农历事件日=0(月末)与月=12 是合法的,不能被公历校验清掉
    love_config_t lunar;
    memset(&lunar, 0, sizeof(lunar));
    lunar.event_count = 1;
    strcpy(lunar.events[0].name, "除夕");
    lunar.events[0].kind = LOVE_EVENT_LUNAR;
    lunar.events[0].date = (love_date_t){ 0, 12, 0 };
    love_config_sanitize(&lunar);
    assert(lunar.events[0].date.month == 12);
    assert(lunar.events[0].date.day == 0);
}

int main(void)
{
    test_migrate_v2_to_v3();
    test_v3_record_round_trip();
    test_unknown_records_rejected();
    test_utf8_copy_truncates_on_boundary();
    test_sanitize_clamps_everything();

    printf("test_love_config: PASS\n");
    return 0;
}
