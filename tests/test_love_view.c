// tests/test_love_view.c —— 轮播模型(love_view)的主机测试。
//
// 这一层钉的是"上/下键走到哪一页、页码写几":错一位就会表现为页码跳过一页、
// 或者停在一条已被删掉的空页上。设备端没有自动化测试,所以边界只能在这里挡住。
#include <assert.h>
#include <stdio.h>

#include "love_view.h"

// 设备上真的用的一屏几条(与 love_app.c 的 LIST_PAGE_ROWS 一致)。
#define ROWS 4

// 逐个走一遍环,把经过的位置收进 slots;返回走了几格。
static int walk(int start, int delta, int total, int *slots, int max)
{
    int count = 0;
    int slot = start;
    // 环上最多 total 个位置,再多走一格必然回主页;上限只是为了别写出死循环。
    for (int guard = 0; guard <= total + 2; guard++) {
        slot = love_view_step(slot, delta, total);
        if (count < max) slots[count] = slot;
        count++;
        if (slot == LOVE_VIEW_HOME) break;
    }
    return count;
}

static void expect_walk(int start, int delta, int total, const int *expected, int expected_count)
{
    int slots[16] = { 0 };
    const int count = walk(start, delta, total, slots, 16);
    if (count != expected_count) {
        printf("走的格数不符: 实际 %d, 期望 %d(起点 %d, 步长 %d, 总页 %d)\n",
               count, expected_count, start, delta, total);
    }
    assert(count == expected_count);
    for (int i = 0; i < count && i < 16; i++) {
        if (slots[i] != expected[i]) {
            printf("第 %d 格不符: 实际 %d, 期望 %d\n", i, slots[i], expected[i]);
        }
        assert(slots[i] == expected[i]);
    }
}

// 一页都没有:环是空的,上/下 不该把用户送到别处去。
static void test_empty_ring(void)
{
    assert(love_view_list_pages(0, ROWS) == 0);
    assert(love_view_list_pages(-3, ROWS) == 0);
    assert(love_view_total(0, 0, ROWS) == 0);
    assert(love_view_total(-1, -1, ROWS) == 0);

    love_view_pos_t pos = { LOVE_VIEW_KIND_CARD, 99 };
    assert(!love_view_at(0, 0, 0, ROWS, &pos));
    assert(!love_view_at(LOVE_VIEW_HOME, 0, 0, ROWS, &pos));
    assert(!love_view_at(0, 0, 0, ROWS, NULL));

    assert(love_view_slot(LOVE_VIEW_KIND_CARD, 0, 0, 0, ROWS) == LOVE_VIEW_HOME);
    assert(love_view_slot(LOVE_VIEW_KIND_LIST, 0, 0, 0, ROWS) == LOVE_VIEW_HOME);

    // 空环:不管从哪走、走到哪,结果都是"留在主页"。
    assert(love_view_step(LOVE_VIEW_HOME, 1, 0) == LOVE_VIEW_HOME);
    assert(love_view_step(LOVE_VIEW_HOME, -1, 0) == LOVE_VIEW_HOME);
    assert(love_view_step(0, 1, 0) == LOVE_VIEW_HOME);
}

// 非法行数不能除零,也不该造出页码。
static void test_bad_rows(void)
{
    assert(love_view_list_pages(7, 0) == 0);
    assert(love_view_list_pages(7, -4) == 0);
    assert(love_view_total(1, 7, 0) == 1);   // 单页卡还在,列表页没了
}

// 一屏 4 条的向上取整:正好整除、差一条、超出一条。
static void test_list_pages_round_up(void)
{
    assert(love_view_list_pages(1, ROWS) == 1);
    assert(love_view_list_pages(4, ROWS) == 1);
    assert(love_view_list_pages(5, ROWS) == 2);
    assert(love_view_list_pages(7, ROWS) == 2);
    assert(love_view_list_pages(8, ROWS) == 2);
    assert(love_view_list_pages(9, ROWS) == 3);
}

// 设备上真实的配置:一个单页事件(咕咕嘎嘎)+ 七条列表事件(七个节日)。
// 轮播 = 单页卡 1 页 + 列表 2 页 = 3 页,与用户要求的"主页-单页-列表页-主页"一致。
static void test_real_device_configuration(void)
{
    const int total = love_view_total(1, 7, ROWS);
    assert(total == 3);

    love_view_pos_t pos = { LOVE_VIEW_KIND_CARD, 0 };
    assert(love_view_at(0, 1, 7, ROWS, &pos));
    assert(pos.kind == LOVE_VIEW_KIND_CARD && pos.index == 0);
    assert(love_view_at(1, 1, 7, ROWS, &pos));
    assert(pos.kind == LOVE_VIEW_KIND_LIST && pos.index == 0);
    assert(love_view_at(2, 1, 7, ROWS, &pos));
    assert(pos.kind == LOVE_VIEW_KIND_LIST && pos.index == 1);
    assert(!love_view_at(3, 1, 7, ROWS, &pos));      // 一共只有 3 页

    // 下键一路走到底:主页 -> 单页卡 -> 列表第 1 页 -> 列表第 2 页 -> 主页。
    const int forward[] = { 0, 1, 2, LOVE_VIEW_HOME };
    expect_walk(LOVE_VIEW_HOME, 1, total, forward, 4);
    // 上键反向:主页 -> 最后一页 -> ... -> 主页。
    const int backward[] = { 2, 1, 0, LOVE_VIEW_HOME };
    expect_walk(LOVE_VIEW_HOME, -1, total, backward, 4);

    // 页码就是"走到第几格":单页卡 1/3、列表两页 2/3 与 3/3(设备端据此写标签)。
    for (int slot = 0; slot < total; slot++) {
        love_view_pos_t at = { LOVE_VIEW_KIND_CARD, 0 };
        assert(love_view_at(slot, 1, 7, ROWS, &at));
        assert(love_view_slot(at.kind, at.index, 1, 7, ROWS) == slot);
    }
}

// slot 与 at 必须互为逆运算,否则"渲染用的是 A、按键算的是 B"会错位。
static void test_at_and_slot_are_inverse(void)
{
    for (int cards = 0; cards <= 3; cards++) {
        for (int list = 0; list <= 9; list++) {
            const int total = love_view_total(cards, list, ROWS);
            assert(total == cards + love_view_list_pages(list, ROWS));
            for (int slot = 0; slot < total; slot++) {
                love_view_pos_t pos = { LOVE_VIEW_KIND_CARD, -1 };
                assert(love_view_at(slot, cards, list, ROWS, &pos));
                assert(love_view_slot(pos.kind, pos.index, cards, list, ROWS) == slot);
            }
            // 越界的 slot / index 都是"不在环上"。
            love_view_pos_t pos = { LOVE_VIEW_KIND_CARD, 0 };
            assert(!love_view_at(total, cards, list, ROWS, &pos));
            assert(!love_view_at(total + 5, cards, list, ROWS, &pos));
            assert(love_view_slot(LOVE_VIEW_KIND_CARD, cards, cards, list, ROWS) == LOVE_VIEW_HOME);
            assert(love_view_slot(LOVE_VIEW_KIND_LIST, love_view_list_pages(list, ROWS),
                                  cards, list, ROWS) == LOVE_VIEW_HOME);
            assert(love_view_slot(LOVE_VIEW_KIND_CARD, -1, cards, list, ROWS) == LOVE_VIEW_HOME);
            assert(love_view_slot((love_view_kind_t)7, 0, cards, list, ROWS) == LOVE_VIEW_HOME);
        }
    }
}

// 只有单页事件 / 只有列表事件:两种极端配置下环也要闭合。
static void test_one_kind_only(void)
{
    // 只有单页事件:没有列表页,最后一页是最后一张卡。
    assert(love_view_total(3, 0, ROWS) == 3);
    const int cards_only[] = { 0, 1, 2, LOVE_VIEW_HOME };
    expect_walk(LOVE_VIEW_HOME, 1, 3, cards_only, 4);
    assert(love_view_slot(LOVE_VIEW_KIND_LIST, 0, 3, 0, ROWS) == LOVE_VIEW_HOME);

    // 只有列表事件:环上全是列表页,主页按"下"进列表第 1 页。
    assert(love_view_total(0, 9, ROWS) == 3);
    const int list_only[] = { 0, 1, 2, LOVE_VIEW_HOME };
    expect_walk(LOVE_VIEW_HOME, 1, 3, list_only, 4);
    assert(love_view_slot(LOVE_VIEW_KIND_CARD, 0, 0, 9, ROWS) == LOVE_VIEW_HOME);

    // 只有一页时,上下两个方向都直接回主页(不会自己绕回自己)。
    assert(love_view_step(0, 1, 1) == LOVE_VIEW_HOME);
    assert(love_view_step(0, -1, 1) == LOVE_VIEW_HOME);
    assert(love_view_step(LOVE_VIEW_HOME, 1, 1) == 0);
    assert(love_view_step(LOVE_VIEW_HOME, -1, 1) == 0);
}

// 后台把某条事件从"单页"改成"列表"(或删掉)之后,原来那张卡就不在环上了。
static void test_slot_leaves_the_ring(void)
{
    // 单页组从 2 条变成 1 条:原来第 2 张卡(下标 1)已经不在环上。
    assert(love_view_slot(LOVE_VIEW_KIND_CARD, 1, 2, 4, ROWS) == 1);
    assert(love_view_slot(LOVE_VIEW_KIND_CARD, 1, 1, 4, ROWS) == LOVE_VIEW_HOME);
    // 列表删到只剩一页:原来的第 2 个列表页也不在环上了。
    assert(love_view_slot(LOVE_VIEW_KIND_LIST, 1, 1, 5, ROWS) == 2);
    assert(love_view_slot(LOVE_VIEW_KIND_LIST, 1, 1, 4, ROWS) == LOVE_VIEW_HOME);
}

// delta 为 0 或非 ±1 时也不该乱跳。
static void test_step_edges(void)
{
    assert(love_view_step(2, 0, 5) == 2);
    assert(love_view_step(LOVE_VIEW_HOME, 0, 5) == LOVE_VIEW_HOME);
    assert(love_view_step(9, 1, 5) == LOVE_VIEW_HOME);      // 早就越界了
    assert(love_view_step(1, 3, 5) == 4);
    assert(love_view_step(1, 4, 5) == LOVE_VIEW_HOME);      // 一步跨过尾端 = 回主页
    assert(love_view_step(3, -3, 5) == 0);
    assert(love_view_step(3, -4, 5) == LOVE_VIEW_HOME);
}

int main(void)
{
    test_empty_ring();
    test_bad_rows();
    test_list_pages_round_up();
    test_real_device_configuration();
    test_at_and_slot_are_inverse();
    test_one_kind_only();
    test_slot_leaves_the_ring();
    test_step_edges();

    printf("test_love_view: PASS\n");
    return 0;
}
