// main/love_store.c —— NVS 持久化实现。
#include "love_store.h"

#include "love_pixel_art.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "love_store";

#define LOVE_NVS_NAMESPACE "love"
#define KEY_CONFIG "cfg"
#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PASS "wifi_pass"
#define KEY_TIME "time"
#define KEY_DAYS_CACHE "days"
#define KEY_AVATAR_PREFIX "av"    // NVS key 上限 15 字节:av0..av3

// 配置写盘时带版本号,后续结构变化可以识别而不是误读旧数据。
// v2:加入 blank_off_seconds(自动熄屏秒数)。
#define LOVE_CONFIG_VERSION 2u

// 自动熄屏档位(秒);0 = 常亮。顺序与设置页的档位标签一一对应,不要单独调整。
const uint16_t LOVE_BLANK_OFF_SECONDS[LOVE_BLANK_OFF_COUNT] = { 15, 30, 60, 180, 0 };

bool love_blank_off_valid(uint16_t seconds)
{
    for (size_t i = 0; i < LOVE_BLANK_OFF_COUNT; i++) {
        if (LOVE_BLANK_OFF_SECONDS[i] == seconds) return true;
    }
    return false;
}

typedef struct {
    uint32_t version;
    love_config_t config;
} config_record_t;

typedef struct {
    uint64_t epoch_seconds;
    uint8_t source;
} time_record_t;

// 断电前的倒计时快照。device 没有 RTC,开机到对上时之间只能靠它显示,
// 所以连同"当时的 UTC 秒"一起存,便于界面判断数据有多旧。
typedef struct {
    uint32_t version;
    int32_t days;
    uint64_t epoch_seconds;   // 0 = 无有效快照
} days_cache_t;

#define DAYS_CACHE_VERSION 1u

typedef struct {
    uint32_t version;
    uint8_t data[LOVE_AVATAR_BYTES];
} avatar_record_t;

#define AVATAR_VERSION 1u

static bool s_ready;

static void copy_name(char *dst, const char *src, size_t size)
{
    if (!src || size == 0) return;
    size_t len = strlen(src);
    if (len >= size) len = size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void love_config_defaults(love_config_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));

    // 与用户提供的截图一致:2000-01-01 在一起,默认两人 + 四个节日 + 一个生日。
    cfg->start.year = 2026;
    cfg->start.month = 8;
    cfg->start.day = 13;

    copy_name(cfg->people[0].name, "咕咕", sizeof(cfg->people[0].name));
    cfg->people[0].icon = LOVE_ICON_BIRD;
    copy_name(cfg->people[1].name, "嘎嘎", sizeof(cfg->people[1].name));
    cfg->people[1].icon = LOVE_ICON_CAT;

    // 默认给三个固定的公历节日 + 生日,再补三个农历节日(春节/中秋/端午),
    // 让"农历"这个能力一开机就能看见,不用用户自己去配。
    const struct {
        const char *name;
        uint8_t icon;
        love_event_kind_t kind;
        int month;
        int day;
    } DEFAULTS[] = {
        { "元旦",      LOVE_ICON_BALLOON, LOVE_EVENT_YEARLY, 1,  1 },
        { "情人节",    LOVE_ICON_HEART,   LOVE_EVENT_YEARLY, 2,  14 },
        { "咕咕嘎嘎",  LOVE_ICON_CAKE,    LOVE_EVENT_YEARLY, 7,  15 },
        { "国庆节",    LOVE_ICON_STAR,    LOVE_EVENT_YEARLY, 10, 1 },
        { "圣诞节",    LOVE_ICON_TREE,    LOVE_EVENT_YEARLY, 12, 25 },
        { "春节",      LOVE_ICON_GIFT,    LOVE_EVENT_LUNAR,  1,  1 },
        { "中秋",      LOVE_ICON_MOON,    LOVE_EVENT_LUNAR,  8,  15 },
        { "端午",      LOVE_ICON_LEAF,    LOVE_EVENT_LUNAR,  5,  5 },
    };
    const size_t count = sizeof(DEFAULTS) / sizeof(DEFAULTS[0]);
    for (size_t i = 0; i < count && i < LOVE_EVENT_MAX; i++) {
        love_event_t *event = &cfg->events[i];
        copy_name(event->name, DEFAULTS[i].name, sizeof(event->name));
        event->icon = DEFAULTS[i].icon;
        event->kind = (uint8_t)DEFAULTS[i].kind;
        event->date.year = cfg->start.year;
        event->date.month = (int8_t)DEFAULTS[i].month;
        event->date.day = (int8_t)DEFAULTS[i].day;
    }
    cfg->event_count = (uint8_t)count;
    cfg->blank_off_seconds = LOVE_BLANK_OFF_DEFAULT;   // 出厂默认档位,任意键唤醒
}

// 载入的数据可能来自旧版本或被写坏,这里统一做一次合法性收敛。
static void sanitize_config(love_config_t *cfg)
{
    if (!love_date_valid(cfg->start)) {
        cfg->start = (love_date_t){ 2000, 1, 1 };
    }
    for (size_t i = 0; i < LOVE_PERSON_MAX; i++) {
        cfg->people[i].name[LOVE_NAME_MAX - 1] = '\0';
        if (cfg->people[i].name[0] == '\0') {
            copy_name(cfg->people[i].name, i == 0 ? "我" : "TA",
                      sizeof(cfg->people[i].name));
        }
        if (cfg->people[i].icon >= LOVE_ICON_TOTAL) cfg->people[i].icon = 0;
    }
    // 熄屏秒数只接受已知档位,别的一律回到默认档。
    if (!love_blank_off_valid(cfg->blank_off_seconds)) {
        cfg->blank_off_seconds = LOVE_BLANK_OFF_DEFAULT;
    }
    if (cfg->event_count > LOVE_EVENT_MAX) cfg->event_count = LOVE_EVENT_MAX;
    for (size_t i = 0; i < cfg->event_count; i++) {
        love_event_t *event = &cfg->events[i];
        event->name[LOVE_NAME_MAX - 1] = '\0';
        if (event->name[0] == '\0') copy_name(event->name, "纪念日", sizeof(event->name));
        if (event->icon >= LOVE_ICON_TOTAL) event->icon = 0;
        if (event->kind > LOVE_EVENT_LUNAR) event->kind = LOVE_EVENT_YEARLY;
        if (event->kind == LOVE_EVENT_LUNAR) {
            // 农历事件:month/day 是农历月日,day = 0 表示月末(除夕)。
            // 这里不能按公历校验(date_valid 会要求具体年月日),单独收敛。
            if (event->date.month < 1 || event->date.month > 12) event->date.month = 1;
            if (event->date.day < 0 || event->date.day > 30) event->date.day = 1;
            continue;   // 跳过下面的公历日期校验
        }
        if (!love_date_valid(event->date)) {
            event->date = (love_date_t){ cfg->start.year, 1, 1 };
        }
    }
}

esp_err_t love_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 仅本应用的配置存在这个分区;分区内容不可用时重建才能继续使用。
        ESP_LOGW(TAG, "NVS 不可用(%s),擦除后重建", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    love_config_t cfg;
    love_store_load_config(&cfg);   // 首次启动时把默认值落盘,便于后台直接改
    s_ready = true;
    return ESP_OK;
}

void love_store_load_config(love_config_t *cfg)
{
    if (!cfg) return;
    love_config_defaults(cfg);

    if (!s_ready) return;

    nvs_handle_t handle;
    if (nvs_open(LOVE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;

    config_record_t record;
    size_t size = sizeof(record);
    esp_err_t err = nvs_get_blob(handle, KEY_CONFIG, &record, &size);
    if (err == ESP_OK && size == sizeof(record) && record.version == LOVE_CONFIG_VERSION) {
        *cfg = record.config;
    } else if (err == ESP_OK) {
        ESP_LOGW(TAG, "配置记录版本或长度不符,回落到默认值");
    }
    nvs_close(handle);

    sanitize_config(cfg);
}

esp_err_t love_store_save_config(const love_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    config_record_t record = { .version = LOVE_CONFIG_VERSION };
    record.config = *cfg;
    sanitize_config(&record.config);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, KEY_CONFIG, &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGE(TAG, "保存配置失败: %s", esp_err_to_name(err));
    return err;
}

esp_err_t love_store_load_wifi(char *ssid, size_t ssid_size,
                               char *pass, size_t pass_size)
{
    if (!s_ready || !ssid || !pass || ssid_size == 0 || pass_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ssid[0] = '\0';
    pass[0] = '\0';

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t size = ssid_size;
    err = nvs_get_str(handle, KEY_WIFI_SSID, ssid, &size);
    if (err == ESP_OK) {
        size = pass_size;
        err = nvs_get_str(handle, KEY_WIFI_PASS, pass, &size);
    }
    nvs_close(handle);
    return err;
}

esp_err_t love_store_save_wifi(const char *ssid, const char *pass)
{
    if (!ssid || !pass) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (strlen(ssid) == 0 || strlen(ssid) >= LOVE_WIFI_SSID_MAX ||
        strlen(pass) >= LOVE_WIFI_PASS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, KEY_WIFI_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_WIFI_PASS, pass);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    // 不在日志里输出 SSID/密码。
    if (err != ESP_OK) ESP_LOGE(TAG, "保存 Wi-Fi 凭据失败: %s", esp_err_to_name(err));
    return err;
}

esp_err_t love_store_clear_wifi(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    (void)nvs_erase_key(handle, KEY_WIFI_SSID);
    (void)nvs_erase_key(handle, KEY_WIFI_PASS);
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t love_store_save_time(uint64_t epoch_seconds, love_time_src_t src)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    const time_record_t record = { epoch_seconds, (uint8_t)src };
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, KEY_TIME, &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool love_store_load_time(uint64_t *epoch_seconds, love_time_src_t *src)
{
    if (epoch_seconds) *epoch_seconds = 0;
    if (src) *src = LOVE_TIME_SRC_NONE;
    if (!s_ready) return false;

    nvs_handle_t handle;
    if (nvs_open(LOVE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    time_record_t record = { 0, 0 };
    size_t size = sizeof(record);
    esp_err_t err = nvs_get_blob(handle, KEY_TIME, &record, &size);
    nvs_close(handle);

    if (err != ESP_OK || size != sizeof(record) || record.epoch_seconds == 0) return false;
    if (record.source > (uint8_t)LOVE_TIME_SRC_BLE) record.source = (uint8_t)LOVE_TIME_SRC_NONE;
    if (epoch_seconds) *epoch_seconds = record.epoch_seconds;
    if (src) *src = (love_time_src_t)record.source;
    return true;
}

/* ---------- 断电前的倒计时快照 ---------- */

esp_err_t love_store_save_days_cache(int32_t days, uint64_t epoch_seconds)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    const days_cache_t record = { DAYS_CACHE_VERSION, days, epoch_seconds };
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, KEY_DAYS_CACHE, &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool love_store_load_days_cache(int32_t *days, uint64_t *epoch_seconds)
{
    if (days) *days = 0;
    if (epoch_seconds) *epoch_seconds = 0;
    if (!s_ready) return false;

    nvs_handle_t handle;
    if (nvs_open(LOVE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    days_cache_t record = { 0, 0, 0 };
    size_t size = sizeof(record);
    esp_err_t err = nvs_get_blob(handle, KEY_DAYS_CACHE, &record, &size);
    nvs_close(handle);

    if (err != ESP_OK || size != sizeof(record)) return false;
    if (record.version != DAYS_CACHE_VERSION) return false;
    if (record.epoch_seconds == 0) return false;

    if (days) *days = record.days;
    if (epoch_seconds) *epoch_seconds = record.epoch_seconds;
    return true;
}

/* ---------- 自定义头像 ---------- */

static void avatar_key(uint8_t slot, char *out, size_t size)
{
    snprintf(out, size, "%s%u", KEY_AVATAR_PREFIX, (unsigned)slot);
}

esp_err_t love_store_save_avatar(uint8_t slot, const void *data)
{
    if (slot >= LOVE_AVATAR_MAX || !data) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    avatar_record_t record = { .version = AVATAR_VERSION };
    memcpy(record.data, data, LOVE_AVATAR_BYTES);

    char key[8];
    avatar_key(slot, key, sizeof(key));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, key, &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGE(TAG, "保存头像 %u 失败: %s", (unsigned)slot, esp_err_to_name(err));
    return err;
}

size_t love_store_load_avatar(uint8_t slot, void *out, size_t out_size)
{
    if (slot >= LOVE_AVATAR_MAX || !out || out_size < LOVE_AVATAR_BYTES) return 0;
    if (!s_ready) return 0;

    char key[8];
    avatar_key(slot, key, sizeof(key));

    nvs_handle_t handle;
    if (nvs_open(LOVE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0;

    avatar_record_t record;
    size_t size = sizeof(record);
    esp_err_t err = nvs_get_blob(handle, key, &record, &size);
    nvs_close(handle);

    if (err != ESP_OK || size != sizeof(record) || record.version != AVATAR_VERSION) return 0;
    memcpy(out, record.data, LOVE_AVATAR_BYTES);
    return LOVE_AVATAR_BYTES;
}

esp_err_t love_store_clear_avatar(uint8_t slot)
{
    if (slot >= LOVE_AVATAR_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    char key[8];
    avatar_key(slot, key, sizeof(key));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    (void)nvs_erase_key(handle, key);
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

