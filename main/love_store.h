// main/love_store.h —— 纪念日摆件的持久化层(NVS)。
//
// 保存三类数据:倒计时配置(起始日、两个人、事件列表)、Wi-Fi 凭据、
// 最后一次成功对时的时间与来源。设备无 RTC,重启后靠这里的时间推算。
#pragma once

#include "esp_err.h"
#include "love_config.h"
#include "love_date.h"

// 时间来源,用于界面提示“时间从哪来”。**这些值会存进 NVS**,只能往后追加,
// 不能改动或删除已有项的数值 —— 否则老设备里已经写下的来源会被读成别的意思。
typedef enum {
    LOVE_TIME_SRC_NONE = 0,
    LOVE_TIME_SRC_SNTP,
    LOVE_TIME_SRC_WEB,
    // 老固件把 BLE 对时写进来的值。设备改为 BLE 串口控制台后这条路已经没了,
    // 但历史记录里可能还是它,保留数值并在界面上与串口对时显示同一个词。
    LOVE_TIME_SRC_BLE,
    LOVE_TIME_SRC_CONSOLE,   // 命令行控制台(USB 或 BLE 串口)写入的时间戳
} love_time_src_t;

#define LOVE_WIFI_SSID_MAX 33
#define LOVE_WIFI_PASS_MAX 65
// 最多记住几个热点。定 5 是"家里 + 公司 + 两台手机热点 + 一个备用"的量级:
// 再多也没人管得过来,而且每个候选都要花掉一次连接尝试的时间。
#define LOVE_WIFI_MAX 5

// 一条已保存的 Wi-Fi 凭据。密码只写入、不读出用于展示(连接时由 love_net 回读)。
typedef struct {
    char ssid[LOVE_WIFI_SSID_MAX];
    char pass[LOVE_WIFI_PASS_MAX];
} love_wifi_cred_t;

// 初始化 NVS。**不含载入**:配置由调用方自己 love_store_load_config() 取到它的状态里
// (缺失时那份接口会给出默认值,不会往 NVS 里写东西)。
esp_err_t love_store_init(void);

// 出厂默认配置。实现留在本文件对应的 .c 里:默认事件要写 LOVE_ICON_BIRD 这类
// 图标序号宏,而它们所在的 love_pixel_art.h 包含 lvgl.h,纯逻辑文件不能碰。
void love_config_defaults(love_config_t *cfg);

// 载入/保存倒计时配置。load 失败时返回默认值,不返回错误。
void love_store_load_config(love_config_t *cfg);
esp_err_t love_store_save_config(const love_config_t *cfg);

// Wi-Fi 凭据列表,**按保存顺序**排列(越靠前越先被尝试)。返回条数,0 = 从没配过网。
//
// 老固件只存一条(键 wifi_ssid/wifi_pass):首次读到那种布局时自动搬进列表并擦掉
// 旧键。搬迁是幂等的 —— 写新格式失败就不擦旧键,下次开机再搬一遍,凭据不会丢。
size_t love_store_load_wifi_list(love_wifi_cred_t *out, size_t max);
esp_err_t love_store_save_wifi_list(const love_wifi_cred_t *list, size_t count);
// 清空整个列表(连老格式的两个键一起擦,否则搬迁逻辑会把它们又捡回来)。
esp_err_t love_store_clear_wifi(void);

// "用户手动关掉了后台热点"这一意图位。设备必须尊重它:手动关掉之后,即使联网失败
// 也不再自动把热点开回来(见 love_net_poll)。用独立的 NVS 键而不是塞进 love_config_t,
// 是为了不动配置记录的布局 —— 改那个结构要连带升版本号与写迁移,风险大得多。
esp_err_t love_store_save_ap_off(bool off);
bool love_store_load_ap_off(void);   // 没写过时返回 false(默认允许自动开热点)

// 热点密码。**每台随机生成一次后持久化**:原先由 MAC 推导(SSID 后两字节 -> 密码),
// 于是任何能看到 SSID 的人都能算出密码,再进没有任何鉴权的后台页。密码只在设备屏幕上
// 显示给主人看,不走这条推导。首次(含老固件升上来)生成并落盘。
// 返回 ESP_OK 表示 out 里是可用的密码。
esp_err_t love_store_load_ap_pass(char *out, size_t size);

// 调试模式闸门:开着时设备不熄屏、不自动深睡,直到主人明确关掉(见 love_app)。
// 同样走独立键,避免动配置记录布局。
esp_err_t love_store_save_debug_mode(bool on);
bool love_store_load_debug_mode(void);

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
