// main/love_event_order.c —— 显示序实现,规则见 love_event_order.h。
#include "love_event_order.h"

#include <limits.h>
#include <string.h>

// "未分类"的组键。比任何真实组键(首次出现的下标,最大 LOVE_EVENT_MAX-1)都大,
// 因此它恒排最后,不需要在比较里为它单开一个分支。
#define GROUP_LAST 0xFF

// 组键 = 同分类中第一条在持久数组里的下标;空分类固定为 GROUP_LAST。
static uint8_t group_key_of(const love_event_t *events, size_t index)
{
    if (events[index].category[0] == '\0') return GROUP_LAST;
    for (size_t i = 0; i < index; i++) {
        if (strcmp(events[i].category, events[index].category) == 0) {
            return (uint8_t)i;
        }
    }
    return (uint8_t)index;   // 本分类第一次出现
}

// 组内排序键:距今的天数绝对值,越小越靠前。
// 农历超表时用 INT32_MAX 顶到末尾 —— 那种情况下 love_event_countdown() 给的
// days 是 0,直接拿来比会把它排到最前,看起来像"就是今天"。
static int32_t days_key(const love_event_t *event, love_date_t today, bool holds)
{
    if (!holds) return 0;

    const love_countdown_t countdown = love_event_countdown(event, today);
    if (!countdown.resolved) return INT32_MAX;
    return countdown.days >= 0 ? countdown.days : -countdown.days;
}

size_t love_event_order_build(const love_event_t *events, size_t count,
                              love_date_t today, bool holds,
                              uint8_t *order, size_t order_size)
{
    if (!events || !order || order_size == 0) return 0;

    if (count > LOVE_EVENT_MAX) count = LOVE_EVENT_MAX;
    if (count > order_size) count = order_size;
    if (count == 0) return 0;

    // 键先算好:插入排序会反复比较同一对元素,而 group_key_of 里是 strcmp。
    uint8_t group[LOVE_EVENT_MAX];
    int32_t days[LOVE_EVENT_MAX];
    for (size_t i = 0; i < count; i++) {
        group[i] = group_key_of(events, i);
        days[i] = days_key(&events[i], today, holds);
        order[i] = (uint8_t)i;
    }

    // 稳定插入排序(元素最多 8 个)。比较时"先组后天数",相等则不动 —— 稳定。
    for (size_t i = 1; i < count; i++) {
        const uint8_t current = order[i];
        size_t j = i;
        while (j > 0) {
            const uint8_t previous = order[j - 1];
            const bool in_order = (group[previous] < group[current]) ||
                                  (group[previous] == group[current] &&
                                   days[previous] <= days[current]);
            if (in_order) break;
            order[j] = previous;
            j--;
        }
        order[j] = current;
    }

    return count;
}
