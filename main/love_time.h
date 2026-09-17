// main/love_time.h —— 设备时间服务。
//
// AI Passport 没有 RTC,时间只有三个来源:联网后的 SNTP、后台网页里手机浏览器
// 的当前时间、以及 BLE 客户端写入的时间戳。任一路径写入后都会存进 NVS,之后
// 靠开机以来的微秒计数推算当前时间;断电期间不推进,重启后若尚未对时会明确
// 显示“时间未同步”,不会假装知道日期。
#pragma once

#include "esp_err.h"
#include "love_store.h"

#include <stdbool.h>
#include <stdint.h>

// 东八区偏移,界面与后台都按北京时间展示。
#define LOVE_TZ_OFFSET_SECONDS (8 * 3600)

typedef struct {
    uint64_t epoch_seconds;
    love_time_src_t source;
    bool holds;              // true 表示设备有可用时间
    bool wifi_pending;       // true 表示正在等 SNTP 返回
} love_time_state_t;

// 读 NVS 里的上次对时结果并启动内部上报任务(幂等)。
esp_err_t love_time_init(void);

// 当前时间快照。
void love_time_get(love_time_state_t *state);

// 写入时间(幂等;同一秒内重复写入会跳过 NVS 写盘)。
// source 决定界面上的“时间来源”提示。
esp_err_t love_time_set(uint64_t epoch_seconds, love_time_src_t source);

// 启动 SNTP 轮询(STA 拿到 IP 后调用,可重复调用)。
esp_err_t love_time_sntp_start(void);

// 停止 SNTP(断开 Wi-Fi 或进入配网热点时调用,避免无用重试)。
void love_time_sntp_stop(void);

// 界面文案:例如 “时间 来源:网页对时” / “时间未同步”。
void love_time_describe(const love_time_state_t *state, char *buf, size_t size);

// 时间来源的中文短文案(网络对时/网页对时/蓝牙对时/未同步)。
// 界面与后台 REST 都从这里取,避免同一张映射表写两遍。
const char *love_time_src_text(love_time_src_t source);

// 对时结果监听:时间变化时在 love_time 任务上下文回调,供界面刷新。
typedef void (*love_time_listener_t)(void *ctx);
esp_err_t love_time_add_listener(love_time_listener_t listener, void *ctx);
