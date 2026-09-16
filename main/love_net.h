// main/love_net.h —— Wi-Fi 管理:默认热点 + 联网对时。
//
// 设备按需开热点:从未配网时开机即开(否则没法进后台),已配网时由后台或设置页
// 主动打开,并有空闲自动关闭。联网成功进入 STA 模式并启动 SNTP 对时;
// AP 与 STA 用 APSTA 共存,手机连着热点也能同时让设备上网。
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOVE_NET_SCAN_MAX 12

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
    bool has_credentials;
    char ip[16];
    int rssi;
    char ap_ssid[33];
    char ap_pass[65];
    char sta_ssid[33];     // 已配置的 SSID;密码只写不读
    char site_url[24];     // 后台地址,例如 http://192.168.4.1
} love_net_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} love_net_ap_t;

esp_err_t love_net_init(void);
void love_net_deinit(void);

// 热点开关。开热点时自动进入 APSTA;关热点且无凭据时回到 IDLE。
esp_err_t love_net_ap_start(void);
esp_err_t love_net_ap_stop(void);

// 保存凭据并连接(后台/设置页调用)。ssid 不能为空。
esp_err_t love_net_set_credentials(const char *ssid, const char *pass);

// 清除凭据并断开,回到热点待配网状态。
esp_err_t love_net_forget(void);

void love_net_get_status(love_net_status_t *out);

// 扫描:触发后由调用方轮询 love_net_scan_results。
esp_err_t love_net_scan_start(void);
bool love_net_scan_pending(void);
esp_err_t love_net_scan_results(love_net_ap_t *out, size_t max, size_t *count);

// 热点空闲自动关闭:调用方定期喂心跳,超时后自动关热点省电。
void love_net_ap_touch(void);
void love_net_ap_set_timeout(uint32_t seconds);

// 周期调用(建议 1 秒一次):处理热点空闲超时。
void love_net_poll(void);
