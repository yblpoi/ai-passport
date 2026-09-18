// main/love_view.h —— 机身页面的"轮播"模型(纯逻辑,不依赖 ESP-IDF/LVGL)。
//
// 为什么要单独一层:上/下键该走到哪一页、页码该写几,是这次改动里最容易算错、
// 也最容易被后来者改坏的一段(错一位就是"页码跳过一页"或"停在空页")。设备端只按
// 这里的结果渲染,不自己算。
//
// 机型事实:三个按键(上/下/确定),屏幕 240x320。上/下 是**唯一的翻页方式**。
//
// 轮播的形状(与"页面逻辑:主页-单页-列表页-主页"一一对应):
//
//     主页 ──下──> 单页卡 0 ──> 单页卡 1 ──> ... ──> 列表页 0 ──> 列表页 1 ──> 主页
//      ^                                                                        │
//      └──────────────────────────── 下(到最后一页) ────────────────────────────┘
//
//   1. 单页事件各占一页,顺序在前;列表事件按每页 rows 条切成若干列表页,顺序在后。
//      两组各自的顺序由 love_event_order 决定(组内按远近)。
//   2. **主页在环上**,是环的两端:主页按"下"进第 0 页、按"上"进最后一页;
//      第 0 页按"上"、最后一页按"下"都回主页。所以环是闭合的,不会卡在某一头。
//      另外给"回主页"配了个快捷键:在页面上**连按两次"上"**一步回主页(设备端把它走成
//      "越出顶端",也就是这里的 LOVE_VIEW_HOME,不额外定义新状态)。
//   3. 页码是**整套轮播一套编号**(1..total),单页卡与列表页共用 —— 以前两者各算
//      各的(单页卡报"1/1"、列表报"1/2"),对用户是两套互不相干的数,看不出自己走到
//      哪里了。
//   4. 列表页**没有条目光标**:上/下 只翻页。要看某条事件的完整卡片,就把它在后台
//      网页上设成"单页"展示方式。
#pragma once

#include <stdbool.h>

// 环上的"主页"。上/下越过两端、或者当前位置已经不在环上,都是它。
#define LOVE_VIEW_HOME (-1)

// 一页只装一种东西:单页事件卡,或者列表屏的一页。
typedef enum {
    LOVE_VIEW_KIND_CARD = 0,   // 单页组里第 index 条事件的卡片
    LOVE_VIEW_KIND_LIST = 1,   // 列表组的第 index 页
} love_view_kind_t;

typedef struct {
    love_view_kind_t kind;
    int index;
} love_view_pos_t;

// 列表组按每页 rows 条切成几页。列表为空、或 rows 非法时是 0 页
// —— 0 页表示"环上根本没有列表页",而不是"有一页空的"。
int love_view_list_pages(int list_events, int rows);

// 轮播总页数 = 单页事件数 + 列表页数。0 表示环上一页都没有(只有主页)。
int love_view_total(int card_events, int list_events, int rows);

// 第 slot 页是什么。越界返回 false,out 不动。
bool love_view_at(int slot, int card_events, int list_events, int rows, love_view_pos_t *out);

// 反过来:某一页在环上的位置。不在环上返回 LOVE_VIEW_HOME —— 例如这条事件本来
// 是单页、用户刚在网页上把它改成"列表"展示,或者事件被删了。
int love_view_slot(love_view_kind_t kind, int index, int card_events, int list_events, int rows);

// 从 slot 走 delta 格。越出两端返回 LOVE_VIEW_HOME(= 回主页)。
// slot == LOVE_VIEW_HOME 表示"从主页进环":前进落到第 0 页,后退落到最后一页。
// total <= 0 时永远回主页(环上没东西,按键不该把用户送到别处去)。
int love_view_step(int slot, int delta, int total);
