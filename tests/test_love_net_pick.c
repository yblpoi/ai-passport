// tests/test_love_net_pick.c —— 选网决策(love_net_pick)的主机测试。
//
// 这三个判据决定"设备在多张已保存的热点里连了谁":
//   - 先扫后试:扫到已保存的热点就直连信号最强的那个;
//   - 换下一个候选时,这一轮扫到过的优先(信号强的先),一张都看不见才按保存顺序;
//   - 一轮之内不重复试同一个候选(试过的记进位图),而"等了多久"必须能表达负数。
// 现场表现是"连上了不该连的那个 / 卡在同一个候选上重试",而真机上很难复现,
// 所以边界只能在这里挡住。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "love_net_pick.h"

static void put_ap(love_net_ap_t *ap, const char *ssid, int rssi)
{
    snprintf(ap->ssid, sizeof(ap->ssid), "%s", ssid);
    ap->rssi = (int8_t)rssi;
    ap->secure = true;
}

static void test_next_untried(void)
{
    const char *saved[] = { "home", "office", "phone", "cafe" };
    love_net_ap_t scan[2];

    // 一张都看不见(空扫描结果):按保存顺序退让。
    assert(love_net_pick_next_untried(saved, 4, 0x00, NULL, 0) == 0);
    assert(love_net_pick_next_untried(saved, 4, 0x03, NULL, 0) == 2);
    // 中间空了一个也照样跳过已试过的。
    assert(love_net_pick_next_untried(saved, 4, 0x0b, NULL, 0) == 2);
    // 全试过 = -1(调用方据此进入冷却)。
    assert(love_net_pick_next_untried(saved, 4, 0x0f, NULL, 0) == -1);
    // 条数比位图窄时,只看条数以内的位。
    assert(love_net_pick_next_untried(saved, 2, 0x03, NULL, 0) == -1);
    // 位图只有 8 位:多出来的条数不会越界读。
    assert(love_net_pick_next_untried(saved, 100, 0xff, NULL, 0) == -1);
    // 空列表。
    assert(love_net_pick_next_untried(saved, 0, 0x00, NULL, 0) == -1);

    // 扫到的那张网优先 —— 哪怕它在保存顺序里更靠后。
    put_ap(&scan[0], "cafe", -50);
    put_ap(&scan[1], "office", -35);
    assert(love_net_pick_next_untried(saved, 4, 0x00, scan, 2) == 1);

    // 看得见的里面挑信号最强的:试过 office 之后轮到 cafe,而不是保存顺序里的 home。
    assert(love_net_pick_next_untried(saved, 4, 0x02, scan, 2) == 3);

    // 看得见的都试过了,才回到保存顺序里的第一个没试过的(隐藏 SSID 也走这条路)。
    assert(love_net_pick_next_untried(saved, 4, 0x0a, scan, 2) == 0);

    // 扫描结果里全是别人家的(含大小写不同的同名网):等于"一张都看不见"。
    put_ap(&scan[0], "Home", -30);
    put_ap(&scan[1], "neighbour", -20);
    assert(love_net_pick_next_untried(saved, 4, 0x06, scan, 2) == 0);

    // NULL 的候选条目跳过(不是崩溃,也不是当成匹配)。
    const char *with_null[] = { NULL, "ok" };
    assert(love_net_pick_next_untried(with_null, 2, 0x00, NULL, 0) == 1);
}

static void test_elapsed_ms(void)
{
    // 候选开始之后过了 5 秒。
    assert(love_net_pick_elapsed_ms(5000, 0) == 5000);
    // 同一毫秒里启动、判断:0。
    assert(love_net_pick_elapsed_ms(12345, 12345) == 0);
    // **候选比 now 新**(心跳先取 now,再在同一次迭代里启动候选):必须是负数,
    // 不能下溢成一个巨大的正数。真机现场就是"选网:连接信号最强的 X"紧跟
    // "候选 X:15 秒内没收到任何连接事件",扫到的那张网一整轮都没被真正试过。
    assert(love_net_pick_elapsed_ms(119068, 119069) == -1);
    assert(love_net_pick_elapsed_ms(119068, 119069) < 15000);
    assert(love_net_pick_elapsed_ms(0, 1) == -1);
    // 计数器回绕(49.7 天)按差值算:仍然只有 26 毫秒。
    assert(love_net_pick_elapsed_ms(10, 0xFFFFFFF0u) == 26);
}

static void test_pick_strongest(void)
{
    const char *saved[] = { "home-a", "home-b", "phone" };
    love_net_ap_t scan[4];
    int rssi = 0;

    // 三个都在附近:挑信号最强的那个(下标 2),并把强度报回去。
    put_ap(&scan[0], "home-a", -70);
    put_ap(&scan[1], "home-b", -55);
    put_ap(&scan[2], "phone", -40);
    put_ap(&scan[3], "neighbour", -20);
    assert(love_net_pick_strongest(saved, 3, scan, 4, &rssi) == 2);
    assert(rssi == -40);

    // 邻居家的热点信号再强也不算:只认已保存的。
    put_ap(&scan[2], "neighbour", -30);
    assert(love_net_pick_strongest(saved, 3, scan, 4, &rssi) == 1);
    assert(rssi == -55);

    // 一个都没扫到:-1,强度保持初值,调用方按保存顺序逐个试。
    put_ap(&scan[0], "other", -30);
    put_ap(&scan[1], "other-2", -30);
    rssi = -127;
    assert(love_net_pick_strongest(saved, 3, scan, 2, &rssi) == -1);
    assert(rssi == -127);

    // 空扫描结果与空候选都不崩。
    assert(love_net_pick_strongest(saved, 3, NULL, 0, &rssi) == -1);
    assert(love_net_pick_strongest(saved, 0, scan, 2, &rssi) == -1);
}

static void test_pick_ties_and_duplicates(void)
{
    const char *saved[] = { "first", "second" };
    love_net_ap_t scan[3];
    int rssi = 0;

    // 并列时取先保存的那条:保存顺序就是用户心里的优先级。
    put_ap(&scan[0], "first", -50);
    put_ap(&scan[1], "second", -50);
    assert(love_net_pick_strongest(saved, 2, scan, 2, &rssi) == 0);
    assert(rssi == -50);

    // 同一个 SSID 出现多条(多 BSSID):取其中最强的那条再来比。
    put_ap(&scan[0], "first", -80);
    put_ap(&scan[1], "second", -60);
    put_ap(&scan[2], "first", -55);
    assert(love_net_pick_strongest(saved, 2, scan, 3, &rssi) == 0);
    assert(rssi == -55);
}

static void test_pick_ignores_bogus_entries(void)
{
    const char *saved[] = { NULL, "ok" };
    love_net_ap_t scan[1];
    int rssi = 0;

    // NULL 的候选条目跳过(不是崩溃,也不是当成匹配)。
    put_ap(&scan[0], "ok", -61);
    assert(love_net_pick_strongest(saved, 2, scan, 1, &rssi) == 1);
    assert(rssi == -61);

    // out_rssi 允许传空:调用方只想知道"有没有"。
    assert(love_net_pick_strongest(saved, 2, scan, 1, NULL) == 1);
}

int main(void)
{
    test_next_untried();
    test_elapsed_ms();
    test_pick_strongest();
    test_pick_ties_and_duplicates();
    test_pick_ignores_bogus_entries();
    printf("love_net_pick: 全部通过\n");
    return 0;
}
