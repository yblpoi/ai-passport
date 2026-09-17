// main/love_view.c —— 轮播模型实现,规则见 love_view.h。
#include "love_view.h"

int love_view_list_pages(int list_events, int rows)
{
    if (rows <= 0) return 0;
    if (list_events <= 0) return 0;
    return (list_events + rows - 1) / rows;
}

int love_view_total(int card_events, int list_events, int rows)
{
    if (card_events < 0) card_events = 0;
    return card_events + love_view_list_pages(list_events, rows);
}

bool love_view_at(int slot, int card_events, int list_events, int rows, love_view_pos_t *out)
{
    if (!out || slot < 0) return false;
    if (card_events < 0) card_events = 0;

    const int list_pages = love_view_list_pages(list_events, rows);
    if (slot < card_events) {
        out->kind = LOVE_VIEW_KIND_CARD;
        out->index = slot;
        return true;
    }
    if (slot < card_events + list_pages) {
        out->kind = LOVE_VIEW_KIND_LIST;
        out->index = slot - card_events;
        return true;
    }
    return false;
}

int love_view_slot(love_view_kind_t kind, int index, int card_events, int list_events, int rows)
{
    if (index < 0) return LOVE_VIEW_HOME;
    if (card_events < 0) card_events = 0;

    if (kind == LOVE_VIEW_KIND_CARD) {
        // 单页卡的位置就是它在单页组里的序号。
        return (index < card_events) ? index : LOVE_VIEW_HOME;
    }
    if (kind == LOVE_VIEW_KIND_LIST) {
        return (index < love_view_list_pages(list_events, rows)) ? card_events + index
                                                                 : LOVE_VIEW_HOME;
    }
    return LOVE_VIEW_HOME;
}

int love_view_step(int slot, int delta, int total)
{
    if (total <= 0) return LOVE_VIEW_HOME;
    if (delta == 0) return (slot >= 0 && slot < total) ? slot : LOVE_VIEW_HOME;

    if (slot < 0) {
        // 从主页进环:下键从第 0 页进,上键从最后一页进(两头对称,环才是闭合的)。
        return (delta > 0) ? 0 : total - 1;
    }

    const int next = slot + delta;
    if (next < 0 || next >= total) return LOVE_VIEW_HOME;
    return next;
}
