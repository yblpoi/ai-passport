// main/love_store.h —— 恋爱倒计时的持久化层(NVS)。
//
// 保存三类数据:倒计时配置(起始日、两个人、事件列表)、Wi-Fi 凭据、
// 最后一次成功对时的时间与来源。设备无 RTC,重启后靠这里的时间推算。
#pragma once

#include "esp_err.h"
#include "love_date.h"

// 时间来源,用于界面提示“时间从哪来”。
typedef enum {
    LOVE_TIME_SRC_NONE = 0,
    LOVE_TIME_SRC_SNTP,
    LOVE_TIME_SRC_WEB,
    LOVE_TIME_SRC_BLE,
} love_time_src_t;

#define LOVE_WIFI_SSID_MAX 33
#define LOVE_WIFI_PASS_MAX 65

// 自动熄屏档位(秒),0 表示常亮。设置页的档位标签、配置校验与后台写入校验
// 共用同一张表,档位有变只需改这里。
#define LOVE_BLANK_OFF_COUNT 5
#define LOVE_BLANK_OFF_DEFAULT 30
extern const uint16_t LOVE_BLANK_OFF_SECONDS[LOVE_BLANK_OFF_COUNT];

// seconds 是否属于已知档位。
bool love_blank_off_valid(uint16_t seconds);

// 自定义头像槽位保存的是"定长 4bpp 索引包":40x40 像素、每字节 2 像素、
// 高半字节在前(顺序与 assets/web/admin.html 的 compressAvatar 一致)。
// 每个槽位固定 LOVE_AVATAR_BYTES 字节,便于按槽位随机读写。
#define LOVE_AVATAR_BYTES 800

typedef struct {
    char name[LOVE_NAME_MAX];
    uint8_t icon;
} love_person_t;

typedef struct {
    love_date_t start;                        // 在一起起始日
    love_person_t people[LOVE_PERSON_MAX];    // 主屏顶部的两个人
    uint8_t event_count;                      // 有效事件数,<= LOVE_EVENT_MAX
    uint16_t blank_off_seconds;               // 自动熄屏秒数,0 = 常亮
    love_event_t events[LOVE_EVENT_MAX];      // 事件列表
} love_config_t;

// 初始化 NVS 并载入已保存的配置(缺失时写入默认值)。
esp_err_t love_store_init(void);

// 载入/保存倒计时配置。load 失败时返回默认值,不返回错误。
void love_store_load_config(love_config_t *cfg);
esp_err_t love_store_save_config(const love_config_t *cfg);

// Wi-Fi 凭据。密码只写入、不读出用于展示。
esp_err_t love_store_load_wifi(char *ssid, size_t ssid_size,
                               char *pass, size_t pass_size);
esp_err_t love_store_save_wifi(const char *ssid, const char *pass);
esp_err_t love_store_clear_wifi(void);

// 最后一次成功对时(UTC 秒 + 来源)。
esp_err_t love_store_save_time(uint64_t epoch_seconds, love_time_src_t src);
bool love_store_load_time(uint64_t *epoch_seconds, love_time_src_t *src);

// 断电/关机前的倒计时天数快照。设备没有 RTC,重启后在对上时之前用这份快照显示,
// 避免开机一堆 "--"。快照里同时存下当时的 UTC 秒,便于判断数据有多旧。
esp_err_t love_store_save_days_cache(int32_t days, uint64_t epoch_seconds);
bool love_store_load_days_cache(int32_t *days, uint64_t *epoch_seconds);

// 自定义头像。slot 为 0..LOVE_AVATAR_MAX-1,data 必须恰好 LOVE_AVATAR_BYTES 字节。
esp_err_t love_store_save_avatar(uint8_t slot, const void *data);
// 读出槽位数据;成功返回实际字节数,没有该槽位返回 0。
size_t love_store_load_avatar(uint8_t slot, void *out, size_t out_size);
esp_err_t love_store_clear_avatar(uint8_t slot);
