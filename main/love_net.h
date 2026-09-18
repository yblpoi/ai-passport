// main/love_net.h —— Wi-Fi 管理:**一组**已保存热点 + 默认热点 + 联网对时。
//
// 设备最多记住 5 个热点(见 love_store.h 的 LOVE_WIFI_MAX)。开机与断线后的选网
// 顺序是:先扫描,在已保存的热点里挑信号最强的连;扫不到(包括对方的 SSID 是隐藏的)
// 再按保存顺序逐个试,每个候选 15 秒。整轮都没连上就退避重来(30 秒起,翻倍到 5 分钟),
// 这期间**不重连、不打日志** —— 稳态下串口只会看到"5 分钟一条"的失败汇总。
//
// 设备按需开热点:从未配网时开机即开(否则没法进后台),已配网时由后台或设置页
// 主动打开,并有空闲自动关闭。联网成功进入 STA 模式并启动 SNTP 对时;
// AP 与 STA 用 APSTA 共存,手机连着热点也能同时让设备上网。
//
// 有一条例外贯穿全篇:**用户手动关掉热点之后,设备不再自动把热点开回来**。
// 开机自动开与联网失败兜底这两条自动路径都会让路,直到用户手动开一次(或清除凭据)。
// 闸的状态记在 ap_manual_off 里,并存 NVS,重启与深睡醒来都还算数。
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOVE_NET_SCAN_MAX 12
// 与 love_store.h 的 LOVE_WIFI_MAX 是同一件事(列表长度)。这里再写一份是为了让
// 本头文件不必拖着持久化层的头一起被包含;两处对不上时 love_net.c 有静态断言兜底。
#define LOVE_NET_SAVED_MAX 5

typedef enum {
    LOVE_NET_OFF = 0,      // 服务未启动
    LOVE_NET_IDLE,         // 已启动,无连接、无热点
    LOVE_NET_CONNECTING,   // STA 正在连接
    LOVE_NET_CONNECTED,    // STA 已取得 IP
    LOVE_NET_FAILED,       // 上一次连接失败
} love_net_state_t;

typedef struct {
    love_net_state_t state;
    bool ap_active;
    // 用户手动关掉过热点的闸:为真表示"别自动开热点"。界面据此把"热点已关闭"
    // 讲清楚,不然用户分不清"一会儿就会自己回来"和"关掉了就不会自己回来"。
    bool ap_manual_off;
    bool has_credentials;
    // 已保存的热点个数(0 = 从没配过网)。判断"配没配过网"一律用它,不要看 sta_ssid:
    // 断网重选期间 sta_ssid 可能还空着。
    size_t saved_count;
    char ip[16];
    int rssi;
    char ap_ssid[33];
    char ap_pass[65];
    char sta_ssid[33];     // 当前正在连接/已连上的那个;密码只写不读
    char site_url[24];     // 热点地址,例如 http://192.168.4.1;热点关闭时为空
    char lan_url[24];      // 局域网地址,例如 http://10.0.0.23;未联网时为空
} love_net_status_t;

typedef struct {
    char ssid[33];
    bool current;          // 当前正在连接/已连上的就是这一条
} love_net_saved_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} love_net_ap_t;

esp_err_t love_net_init(void);
void love_net_deinit(void);

// 热点开关。开热点时自动进入 APSTA;关热点且无凭据时回到 IDLE。
// 手动关会记下"用户不要热点"这一意图(写 NVS),此后开机自动开热点与联网失败兜底
// 都不再触发;手动开(或 love_net_forget)把它撤销。空闲超时那次自动关不算手动关。
esp_err_t love_net_ap_start(void);
esp_err_t love_net_ap_stop(void);

// 保存凭据并(在需要时)重新选网。同名 SSID 就地更新密码并保留原来的尝试顺序,
// 新 SSID 追加在列表末尾;列表满(LOVE_NET_SAVED_MAX)时返回 ESP_ERR_INVALID_STATE。
// 改的是"当前这条"会断开重连;改的是别的热点则不动现有连接 —— 加一个备用网络不该
// 把正在用的那个踢掉。
esp_err_t love_net_set_credentials(const char *ssid, const char *pass);

// 删除一条(按 SSID,精确匹配)。找不到返回 ESP_ERR_NOT_FOUND。
// 删的是当前这条会断开重连;删空等于 love_net_forget()(开热点、清 NVS)。
esp_err_t love_net_forget_ssid(const char *ssid);

// 已保存的热点列表(按保存顺序)。返回条数。
size_t love_net_saved_list(love_net_saved_t *out, size_t max);

// 清除全部凭据并断开,回到热点待配网状态。
esp_err_t love_net_forget(void);

void love_net_get_status(love_net_status_t *out);

// 网络状态的中文文案(后台网页与控制台共用同一套,别各写一份)。
const char *love_net_state_text(love_net_state_t state);

// 扫描:触发后由调用方轮询 love_net_scan_results。
esp_err_t love_net_scan_start(void);
bool love_net_scan_pending(void);
esp_err_t love_net_scan_results(love_net_ap_t *out, size_t max, size_t *count);

// 热点空闲自动关闭:调用方定期喂心跳,超时后自动关热点省电。
void love_net_ap_touch(void);

// 周期调用(建议 1 秒一次):处理热点空闲超时。
void love_net_poll(void);
