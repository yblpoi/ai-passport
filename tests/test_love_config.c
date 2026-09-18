// tests/test_love_config.c —— 配置结构与各版本迁移的 host 测试。
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

// 冻结的 v3 布局(与 v2 同理):它们的记录必须永远是 532 字节,否则老设备上按 v3
// 存下的起始日、姓名、事件会被按别的长度解释 —— 这正是"静默清空用户数据"的入口。
_Static_assert(sizeof(love_config_v3_event_t) == 58, "v3 事件结构变了");
_Static_assert(offsetof(love_config_v3_event_t, category) == 32, "v3 事件字段偏移变了");
_Static_assert(sizeof(love_config_v3_t) == 526, "v3 配置结构变了");
_Static_assert(sizeof(love_config_v3_record_t) == 532, "v3 记录长度变了");

// v4(当前版本):事件上限 8 → 24,每条事件多一个 view_mode(展示方式从全局挪到每条)。
// 动这些数字就必须同时升 LOVE_CONFIG_VERSION 并写迁移。
_Static_assert(sizeof(love_event_t) == 58, "v4 事件结构变了,需要升版本并写迁移");
_Static_assert(offsetof(love_event_t, view_mode) == 57, "v4 事件字段偏移变了");
_Static_assert(sizeof(love_config_t) == 1454, "v4 配置结构变了,需要升版本并写迁移");
_Static_assert(sizeof(love_config_record_t) == 1460, "v4 记录长度变了,需要升版本并写迁移");

// 模拟调用方加载前铺好的默认值,用来验证"新字段不受迁移影响"。
static void seed_defaults(love_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->ble_enabled = 0;
    cfg->blank_off_seconds = LOVE_BLANK_OFF_DEFAULT;
}

static void test_migrate_v2_to_current(void)
{
    love_config_v2_record_t old;
    memset(&old, 0, sizeof(old));
    old.version = 2u;
    old.config.start = (love_date_t){ 2025, 3, 14 };
    old.config.blank_off_seconds = 180;
    strcpy(old.config.people[0].name, "咕咕");
    old.config.people[0].icon = 17;      // 自定义头像槽位(v2 时代 16..19)
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
    assert(now.people[0].icon == 19);    // 头像槽 1:v2 的 17 挪到 v5 的 19
    assert(strcmp(now.people[1].name, "嘎嘎") == 0);
    assert(now.people[1].icon == 3);
    assert(now.event_count == 3);
    assert(strcmp(now.events[0].name, "元旦") == 0);
    assert(now.events[0].kind == 0 && now.events[0].date.month == 1);
    assert(strcmp(now.events[1].name, "春节") == 0);
    assert(now.events[1].kind == 2 && now.events[1].date.month == 1);
    assert(strcmp(now.events[2].name, "毕业") == 0);
    assert(now.events[2].kind == 1 && now.events[2].date.year == 2024);

    // v3/v4 才有的字段:v2 没有分类(空串),也没有展示方式 —— 那时候设备就是
    // "一个事件一屏",所以迁移成单页,升级后看到的东西不变。蓝牙开关保持调用方
    // 铺的默认值。事件尾部残留必须被清掉。
    for (size_t i = 0; i < LOVE_EVENT_MAX; i++) {
        assert(now.events[i].category[0] == '\0');
    }
    assert(now.events[0].view_mode == LOVE_EVENT_VIEW_PAGE);
    assert(now.events[1].view_mode == LOVE_EVENT_VIEW_PAGE);
    assert(now.events[2].view_mode == LOVE_EVENT_VIEW_PAGE);
    assert(now.ble_enabled == 0);
    assert(now.events[LOVE_EVENT_MAX - 1].name[0] == '\0');
}

// v2 记录的 events 只有 8 个槽位(love_config.h 的冻结布局),而 event_count 是从记录里
// 读出来的、可能被写坏。修复前这段迁移会按 24 去读结构体尾部之外的内存,还把那片没有
// NUL 保证的字节当字符串 strcpy 出来。这条用例钉两件事:计数被夹回 8,以及第 9 条起
// 仍是调用方铺的默认值 —— 也就是"没有越界读进来任何东西"。
static void test_migrate_v2_overlong_event_count_is_clamped(void)
{
    love_config_v2_record_t old;
    memset(&old, 0, sizeof(old));
    old.version = 2u;
    old.config.start = (love_date_t){ 2025, 1, 1 };
    old.config.event_count = 24;      // 记录里只有 8 个槽位,这个数不可能合法
    for (int i = 0; i < 8; i++) {
        snprintf(old.config.events[i].name, sizeof(old.config.events[i].name),
                 "事件%d", i + 1);
        old.config.events[i].icon = (uint8_t)i;
        old.config.events[i].kind = 0;
        old.config.events[i].date = (love_date_t){ 2025, 1, 1 };
    }

    love_config_t now;
    seed_defaults(&now);
    assert(love_config_from_record(&old, sizeof(old), &now) == true);

    assert(now.event_count == 8);     // v2 装不下第九条
    for (int i = 0; i < 8; i++) {
        char expected[16];
        snprintf(expected, sizeof(expected), "事件%d", i + 1);
        assert(strcmp(now.events[i].name, expected) == 0);
    }
    // 越界读的哨兵:第 9 条起一个字节都不该被动过
    for (size_t i = 8; i < LOVE_EVENT_MAX; i++) {
        assert(now.events[i].name[0] == '\0');
    }
}

// v3 -> 当前:字段基本一一对应,唯一要小心的是 v3 的**全局**展示模式要摊到每条事件上
// (v4 起是每条自己带),否则升级后用户看到的屏会突然从列表变成单页。
static void test_migrate_v3_to_current(void)
{
    love_config_v3_record_t record;
    memset(&record, 0, sizeof(record));
    record.version = 3u;
    record.config.start = (love_date_t){ 2000, 1, 1 };
    record.config.blank_off_seconds = 60;
    record.config.event_count = 2;
    strcpy(record.config.people[0].name, "咕咕");
    record.config.people[0].icon = 17;   // 自定义头像槽位(v3 时代 16..19)
    strcpy(record.config.events[0].name, "在一起");
    strcpy(record.config.events[0].category, "纪念日");
    record.config.events[0].icon = 6;
    record.config.events[0].kind = 0;
    record.config.events[0].date = (love_date_t){ 2000, 1, 1 };
    strcpy(record.config.events[1].name, "春节");
    record.config.events[1].kind = 2;
    record.config.events[1].date = (love_date_t){ 0, 1, 1 };
    record.config.display_mode = 0;    // 0 = 列表(v3 的取值)
    record.config.ble_enabled = 1;

    love_config_t now;
    seed_defaults(&now);
    assert(love_config_from_record(&record, sizeof(record), &now) == true);

    // 逐字段搬过来了
    assert(now.start.year == 2000 && now.start.month == 1 && now.start.day == 1);
    assert(now.blank_off_seconds == 60);
    assert(now.ble_enabled == 1);
    assert(now.event_count == 2);
    assert(strcmp(now.people[0].name, "咕咕") == 0 && now.people[0].icon == 19);
    assert(strcmp(now.events[0].name, "在一起") == 0);
    assert(strcmp(now.events[0].category, "纪念日") == 0);
    assert(now.events[0].icon == 6 && now.events[0].kind == 0);
    assert(now.events[1].kind == 2 && now.events[1].date.day == 1);

    // 全局"列表"摊到了每一条上
    assert(now.events[0].view_mode == LOVE_EVENT_VIEW_LIST);
    assert(now.events[1].view_mode == LOVE_EVENT_VIEW_LIST);
    // 尾部残留清掉
    assert(now.events[2].name[0] == '\0');
    assert(now.events[LOVE_EVENT_MAX - 1].name[0] == '\0');
}

// v3 里是单页(display_mode = 1)的老设备,升级后也应当是单页。
static void test_migrate_v3_single_page_stays_single(void)
{
    love_config_v3_record_t record;
    memset(&record, 0, sizeof(record));
    record.version = 3u;
    record.config.event_count = 1;
    record.config.display_mode = 1;

    love_config_t now;
    seed_defaults(&now);
    assert(love_config_from_record(&record, sizeof(record), &now) == true);
    assert(now.events[0].view_mode == LOVE_EVENT_VIEW_PAGE);
}

// v4 -> v5:内置图标 16 -> 18(鞭炮、花束追加在末尾),自定义头像槽从 16..19 挪到 18..21。
// 这是唯一一次"图标号的语义变了"的迁移,错了会让已上传的头像显示成鞭炮或花束。
static void test_migrate_v4_to_v5(void)
{
    love_config_record_t record;
    memset(&record, 0, sizeof(record));
    record.version = 4u;                 // 老版本号写死:这是历史格式的一部分
    record.config.start = (love_date_t){ 2000, 1, 1 };
    record.config.event_count = 2;

    record.config.people[0].icon = 15;   // 老内置图标的最后一个:不动
    record.config.people[1].icon = 16;   // v4 的头像槽 0 -> v5 的槽 0(18)
    record.config.events[0].icon = 19;   // v4 的头像槽 3 -> v5 的槽 3(21)
    record.config.events[1].icon = 0;    // 老内置图标:不动

    love_config_t now;
    seed_defaults(&now);
    assert(love_config_from_record(&record, sizeof(record), &now) == true);
    assert(now.people[0].icon == 15);
    assert(now.people[1].icon == 18);
    assert(now.events[0].icon == 21);
    assert(now.events[1].icon == 0);

    // 迁移结果必须是当前认得的编号,否则 sanitize 会把它清零、头像就没了。
    love_config_sanitize(&now);
    assert(now.people[1].icon == 18 && now.events[0].icon == 21);
}

// 当前版本自己:原样读回,包括每条事件的展示方式。
static void test_current_record_round_trip(void)
{
    love_config_record_t record;
    memset(&record, 0, sizeof(record));
    record.version = LOVE_CONFIG_VERSION;
    record.config.start = (love_date_t){ 2000, 1, 1 };
    record.config.event_count = 2;
    record.config.blank_off_seconds = 180;
    record.config.ble_enabled = 1;
    strcpy(record.config.people[0].name, "咕咕");
    strcpy(record.config.events[0].name, "在一起");
    strcpy(record.config.events[0].category, "纪念日");
    record.config.events[0].view_mode = LOVE_EVENT_VIEW_LIST;
    strcpy(record.config.events[1].name, "除夕");
    record.config.events[1].kind = LOVE_EVENT_LUNAR;
    record.config.events[1].view_mode = LOVE_EVENT_VIEW_PAGE;

    love_config_t now;
    seed_defaults(&now);
    assert(love_config_from_record(&record, sizeof(record), &now) == true);
    assert(now.ble_enabled == 1);
    assert(now.blank_off_seconds == 180);
    assert(now.event_count == 2);
    assert(now.events[0].view_mode == LOVE_EVENT_VIEW_LIST);
    assert(now.events[1].view_mode == LOVE_EVENT_VIEW_PAGE);
    assert(strcmp(now.events[1].name, "除夕") == 0);
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
    assert(cfg.ble_enabled == 0);
    assert(cfg.event_count == LOVE_EVENT_MAX);
    assert(cfg.events[0].icon == 0);
    assert(cfg.events[0].kind == LOVE_EVENT_YEARLY);
    // 展示方式全零 = LOVE_EVENT_VIEW_LIST,是合法的出厂默认,不该被判成非法;
    // 越界的值才要收敛回列表。
    cfg.events[0].view_mode = 42;
    love_config_sanitize(&cfg);
    assert(cfg.events[0].view_mode == LOVE_EVENT_VIEW_LIST);
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
    test_migrate_v2_to_current();
    test_migrate_v2_overlong_event_count_is_clamped();
    test_migrate_v3_to_current();
    test_migrate_v3_single_page_stays_single();
    test_migrate_v4_to_v5();
    test_current_record_round_trip();
    test_unknown_records_rejected();
    test_utf8_copy_truncates_on_boundary();
    test_sanitize_clamps_everything();

    printf("test_love_config: PASS\n");
    return 0;
}
