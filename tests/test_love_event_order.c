#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "love_event_order.h"

// 固定一个"今天",所有期望值都按它算出来。
#define TODAY_YEAR 2026
#define TODAY_MONTH 9
#define TODAY_DAY 17

static love_date_t today(void)
{
    return (love_date_t){ TODAY_YEAR, TODAY_MONTH, TODAY_DAY };
}

// 公历每年重复的事件。name 留空不影响排序,只有 category 参与分组。
static love_event_t yearly(const char *category, int month, int day)
{
    love_event_t e = { 0 };
    e.kind = LOVE_EVENT_YEARLY;
    e.date.month = (int8_t)month;
    e.date.day = (int8_t)day;
    snprintf(e.category, sizeof(e.category), "%s", category);
    return e;
}

static love_event_t lunar(const char *category, int month, int day)
{
    love_event_t e = yearly(category, month, day);
    e.kind = LOVE_EVENT_LUNAR;
    return e;
}

// 只算这一个具体日期,过期后 days 为负 —— 只有它能造出"已过"的事件,
// 每年重复的事件会被折算到下一次发生日,永远非负。
static love_event_t once(const char *category, int year, int month, int day)
{
    love_event_t e = yearly(category, month, day);
    e.kind = LOVE_EVENT_ONCE;
    e.date.year = (int16_t)year;
    return e;
}

static void expect_order(const uint8_t *order, size_t count, const uint8_t *expected)
{
    for (size_t i = 0; i < count; i++) {
        assert(order[i] == expected[i]);
    }
}

static void test_degenerate_inputs(void)
{
    love_event_t events[2] = { yearly("", 1, 1), yearly("", 2, 2) };
    uint8_t order[LOVE_EVENT_MAX];

    assert(love_event_order_build(NULL, 2, today(), true, order, sizeof(order)) == 0);
    assert(love_event_order_build(events, 2, today(), true, NULL, sizeof(order)) == 0);
    assert(love_event_order_build(events, 2, today(), true, order, 0) == 0);
    assert(love_event_order_build(events, 0, today(), true, order, sizeof(order)) == 0);
}

// 没有分类时全部属于同一组,于是整个列表按"距今远近"升序。
// 这是列表屏抬头就能看到下一个要过的日子的前提。
static void test_no_categories_sorts_by_distance(void)
{
    const love_event_t events[] = {
        yearly("", 9, 20),    // 3 天后
        yearly("", 9, 18),    // 1 天后
        yearly("", 12, 25),   // 99 天后
    };
    uint8_t order[LOVE_EVENT_MAX];

    const size_t count = love_event_order_build(events, 3, today(), true,
                                                order, sizeof(order));
    assert(count == 3);
    const uint8_t expected[] = { 1, 0, 2 };
    expect_order(order, count, expected);
}

// 已过的日子按 |days| 参与排序:7 天前排在 14 天后之前。
static void test_past_events_use_absolute_distance(void)
{
    const love_event_t events[] = {
        yearly("", 10, 1),        // 14 天后
        once("", 2026, 9, 10),    // 7 天前,days = -7
        once("", 2026, 9, 17),    // 今天,距离 0
    };
    uint8_t order[LOVE_EVENT_MAX];

    const size_t count = love_event_order_build(events, 3, today(), true,
                                                order, sizeof(order));
    const uint8_t expected[] = { 2, 1, 0 };
    expect_order(order, count, expected);
}

// 组序 = 分类在持久数组里首次出现的顺序,所以后台上移某分类的第一条就能把整组提前;
// 未分类恒排最后,不管它出现在数组的哪个位置。
static void test_categories_group_in_first_appearance_order(void)
{
    const love_event_t events[] = {
        yearly("生日", 9, 20),   // 组 0,3 天后
        yearly("", 12, 25),      // 未分类
        yearly("节日", 9, 18),   // 组 2,1 天后
        yearly("生日", 10, 1),   // 组 0,14 天后
    };
    uint8_t order[LOVE_EVENT_MAX];

    const size_t count = love_event_order_build(events, 4, today(), true,
                                                order, sizeof(order));
    const uint8_t expected[] = { 0, 3, 2, 1 };
    expect_order(order, count, expected);
}

// 分类名相同才算同一组;大小写与前后缀不同都是不同分类。
static void test_grouping_is_exact_match(void)
{
    const love_event_t events[] = {
        yearly("生日", 9, 20),
        yearly("生日 ", 9, 18),   // 多一个空格 = 另一个分类
    };
    uint8_t order[LOVE_EVENT_MAX];

    const size_t count = love_event_order_build(events, 2, today(), true,
                                                order, sizeof(order));
    // 两个不同组,组序按首次出现,组内只有一条 —— 顺序仍是 0,1,
    // 但两个事件分属两组,所以"生日 "那条的 1 天优势不会把它拉到前面。
    const uint8_t expected[] = { 0, 1 };
    expect_order(order, count, expected);
}

// 农历年份超出数据表时 love_event_countdown() 返回 days = 0 且 resolved = false。
// 不显式处理它就会冒充"今天"排到最前,这条断言就是钉住这个缺陷的。
static void test_unresolved_lunar_goes_last(void)
{
    // 农历表覆盖 2018..2050,取一个更远的"今天"让所有候选年份都查不到。
    const love_date_t far = { 2060, 6, 1 };
    const love_event_t events[] = {
        lunar("", 8, 15),      // 算不出来 -> days 会被填成 0
        yearly("", 10, 1),     // 距今约 120 天
    };
    uint8_t order[LOVE_EVENT_MAX];

    assert(!love_event_countdown(&events[0], far).resolved);

    const size_t count = love_event_order_build(events, 2, far, true,
                                                order, sizeof(order));
    const uint8_t expected[] = { 1, 0 };
    expect_order(order, count, expected);
}

// 还没对上时的时候算不出距离,此时**分组照旧**(分组不需要时间),只是组内保持
// 持久顺序,不给一个假顺序。
static void test_without_time_keeps_persistent_order(void)
{
    const love_event_t events[] = {
        yearly("生日", 10, 1),   // 组 0
        yearly("", 9, 18),       // 未分类
        yearly("生日", 9, 20),   // 组 0
        yearly("节日", 1, 1),    // 组 3
    };
    uint8_t order[LOVE_EVENT_MAX];

    const size_t count = love_event_order_build(events, 4, today(), false,
                                                order, sizeof(order));
    // 组序仍是首次出现(生日 -> 节日 -> 未分类),组内不动:生日那组是 0,2。
    const uint8_t expected[] = { 0, 2, 3, 1 };
    expect_order(order, count, expected);
}

// 同组同距离必须保持原序(稳定),否则每次重绘列表都可能换个样子。
static void test_equal_keys_keep_original_order(void)
{
    const love_event_t events[] = {
        yearly("", 9, 20),
        yearly("", 9, 20),
        yearly("", 9, 20),
    };
    uint8_t order[LOVE_EVENT_MAX];

    const size_t count = love_event_order_build(events, 3, today(), true,
                                                order, sizeof(order));
    const uint8_t expected[] = { 0, 1, 2 };
    expect_order(order, count, expected);
}

static void test_truncates_to_buffer_and_max(void)
{
    // 9 条:超过 LOVE_EVENT_MAX(8),应当被截到 8。
    love_event_t events[9];
    for (int i = 0; i < 9; i++) events[i] = yearly("", 1 + i, 1);

    uint8_t order[LOVE_EVENT_MAX];
    assert(love_event_order_build(events, 9, today(), true, order, sizeof(order)) == LOVE_EVENT_MAX);

    // 缓冲比事件少时,按缓冲大小截断。
    uint8_t small[3];
    assert(love_event_order_build(events, 9, today(), true, small, sizeof(small)) == 3);
}

int main(void)
{
    test_degenerate_inputs();
    test_no_categories_sorts_by_distance();
    test_past_events_use_absolute_distance();
    test_categories_group_in_first_appearance_order();
    test_grouping_is_exact_match();
    test_unresolved_lunar_goes_last();
    test_without_time_keeps_persistent_order();
    test_equal_keys_keep_original_order();
    test_truncates_to_buffer_and_max();

    printf("test_love_event_order: PASS\n");
    return 0;
}
