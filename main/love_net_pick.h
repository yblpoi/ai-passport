// main/love_net_pick.h —— 选网的三个**纯**决策:下一个候选、扫描结果里挑最强的、
// 候选已经等了多久。
//
// 为什么单独成文件:这几个决定会在现场决定"设备到底连了哪个热点",而它们的判据
// (先扫后试、试过的记进位图、看的见的优先、并列时取先保存的那个、比 -127 dBm 还弱的
// 不算、时间差要带符号)是纯逻辑,不该只能靠真机试出来。这里不依赖 ESP-IDF 与 LVGL,
// host tests 可以直接喂数据。
//
// 只放决策:扫描、连接、退避计时都留在 love_net.c —— 那些要真的碰 Wi-Fi 驱动。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 一次扫描到的一个热点。定义在这里(而不是 love_net.h)是为了让决策模块也能看懂它,
// 同时 love_net.h 照旧 include 本头文件把类型暴露出去,调用方不用改包含关系。
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} love_net_ap_t;

// 下一个该试的候选:优先"还没试过、而且在最近一次扫描里看得见"的那些里信号最强的
// 那个;一张都看不见(隐藏 SSID,或扫描那一刻刚好没听见)才退回保存顺序里第一个
// 还没试过的。已试过的记在 tried 位图里(saved_count 最大 8)。返回候选下标;-1 = 都试过了。
int love_net_pick_next_untried(const char *const *saved_ssids, size_t saved_count,
                               uint8_t tried, const love_net_ap_t *scan, size_t scan_count);

// 在已保存的候选里挑"这一轮该优先连"的那个:SSID 出现在扫描结果中、且信号最强的那个。
// 并列时取**先保存**的那条(保存顺序是用户心里的优先级)。写回它的信号强度到 out_rssi。
// 返回候选下标;-1 = 附近一个已保存的热点都没扫到(调用方按保存顺序逐个试)。
int love_net_pick_strongest(const char *const *saved_ssids, size_t saved_count,
                            const love_net_ap_t *scan, size_t scan_count,
                            int *out_rssi);

// 距候选开始过了多少毫秒,**带符号**。
//
// 别把它改回裸的无符号相减:心跳在每次迭代**开头**就把 now 取好,而"扫描结果回来了"
// 会在同一次迭代里启动新候选 —— 那一刻 since 比 now 新,无符号相减下溢成一个巨大的
// 正数,于是刚选出来的候选在几毫秒内就被当成"15 秒一个事件都没来"丢掉。真机日志里
// 成对出现的是"选网:连接信号最强的 X"紧跟"候选 X:15 秒内没收到任何连接事件",
// 结果是扫到的那张网一整轮都没被真正试过(它的"已试"位已经置上了)。
int32_t love_net_pick_elapsed_ms(uint32_t now_ms, uint32_t since_ms);
