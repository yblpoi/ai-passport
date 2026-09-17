// main/love_event_order.h —— 事件列表屏的"显示序"(纯逻辑,不依赖 ESP-IDF/LVGL)。
//
// 为什么要单独一层:列表屏的分组与排序是唯一能空主机测试的部分,而它最容易被
// 改坏(顺序一乱,用户看到的是错的事件)。设备端只按这里算出的**显示序**渲染,
// 绝不改 s_cfg.events[] —— 持久顺序归后台网页的上移/下移按钮管。
//
// 规则:
//   1. 分类非空的事件各自成一组,组间顺序 = 该分类在持久数组里**首次出现**的顺序;
//      因此网页上把某个分类的第一条往上移,就能让整组排到前面。
//   2. 分类为空("未分类")的恒排最后,不分它出现在哪里。
//   3. 组内按"距今远近"升序(最近的在前),这样列表抬头就是下一个要过的日子。
//      resolved == false(农历年份超出数据表)必须显式排到最后 —— 那种情况下
//      love_event_countdown() 返回 days = 0,不单独处理就会冒充"就是今天"排到最前。
//   4. 拿不到时间(未对时)时组内保持持久顺序,不给无法计算的假顺序。
//   5. 同键保持原序(稳定排序)。
#pragma once

#include "love_date.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 算出显示序。order[i] 是 events[] 里的下标,返回实际写入的条数。
// 条数超过 count / order_size / LOVE_EVENT_MAX 时按最小者截断。
// holds 为 false(设备还没有可用时间)时不做组内排序。
//
// view_filter 用来分别取"列表事件"和"单页事件"两组:传 LOVE_EVENT_VIEW_LIST 或
// LOVE_EVENT_VIEW_PAGE 只排那一组,传 LOVE_EVENT_VIEW_ANY 就不过滤。
// 分组键也只看通过筛选的那些事件 —— 否则组序会被一条根本不出现在这一组里的事件
// 决定。
#define LOVE_EVENT_VIEW_ANY 0xFFu

size_t love_event_order_build(const love_event_t *events, size_t count,
                              uint8_t view_filter, love_date_t today, bool holds,
                              uint8_t *order, size_t order_size);
