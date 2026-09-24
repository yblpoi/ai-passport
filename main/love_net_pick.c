// main/love_net_pick.c —— 见 love_net_pick.h。纯函数,不碰任何驱动。
#include "love_net_pick.h"

#include <string.h>

int love_net_pick_next_untried(const char *const *saved_ssids, size_t saved_count,
                               uint8_t tried, const love_net_ap_t *scan, size_t scan_count)
{
    if (saved_count > 8) saved_count = 8;   // tried 是 uint8_t:再多也表达不了
    if (!saved_ssids) return -1;

    // 第一遍:还没试过的候选里,哪一个在最近的扫描结果里看得见、而且信号最强。
    // 这一遍决定了"换下一个"该换到谁 —— 一轮里剩下的候选常常横跨好几个网络环境,
    // 挑看得见的那个才不至于把整轮时间花在已经搬走的热点上。
    int best = -1;
    int best_rssi = -127;
    for (size_t i = 0; i < saved_count; i++) {
        if ((tried & (uint8_t)(1u << i)) != 0) continue;
        if (!saved_ssids[i]) continue;
        for (size_t j = 0; scan && j < scan_count; j++) {
            if (strcmp(saved_ssids[i], scan[j].ssid) != 0) continue;
            if ((int)scan[j].rssi > best_rssi) {
                best_rssi = (int)scan[j].rssi;
                best = (int)i;
            }
        }
    }
    if (best >= 0) return best;

    // 第二遍:一张都看不见(隐藏 SSID,或扫描那一刻刚好没听见),按保存顺序退让 ——
    // 保存顺序是用户心里的优先级。
    for (size_t i = 0; i < saved_count; i++) {
        if ((tried & (uint8_t)(1u << i)) == 0 && saved_ssids[i]) return (int)i;
    }
    return -1;
}

int love_net_pick_strongest(const char *const *saved_ssids, size_t saved_count,
                            const love_net_ap_t *scan, size_t scan_count,
                            int *out_rssi)
{
    int best = -1;
    int best_rssi = -127;
    for (size_t i = 0; i < saved_count; i++) {
        if (!saved_ssids[i]) continue;
        for (size_t j = 0; j < scan_count; j++) {
            if (strcmp(saved_ssids[i], scan[j].ssid) != 0) continue;
            // 严格大于:同一轮里先保存的候选先被看过,所以并列时它保住位置。
            if ((int)scan[j].rssi > best_rssi) {
                best_rssi = (int)scan[j].rssi;
                best = (int)i;
            }
        }
    }
    if (out_rssi) *out_rssi = best_rssi;
    return best;
}

int32_t love_net_pick_elapsed_ms(uint32_t now_ms, uint32_t since_ms)
{
    // 无符号相减 + 转成 int32:既能表达"since 比 now 新"(负数 = 还没开始),
    // 也天然处理计数器回绕(49.7 天)。判据见头文件里的说明。
    return (int32_t)(now_ms - since_ms);
}
