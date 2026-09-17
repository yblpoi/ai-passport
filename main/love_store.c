// main/love_store.c —— NVS 持久化实现。
#include "love_store.h"

#include <stdlib.h>

#include "love_pixel_art.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "love_store";

#define LOVE_NVS_NAMESPACE "love"
#define KEY_CONFIG "cfg"
#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PASS "wifi_pass"
#define KEY_AP_OFF "ap_off"
#define KEY_AP_PASS "ap_pass"
#define KEY_TIME "time"
#define KEY_DAYS_CACHE "days"
#define KEY_AVATAR_PREFIX "av"    // NVS key 上限 15 字节:av0..av3

// 配置写盘时带版本号,后续结构变化可以识别而不是误读旧数据。
// v2:加入 blank_off_seconds(自动熄屏秒数)。

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

void love_config_defaults(love_config_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));

    // 与用户提供的截图一致:2000-01-01 在一起,默认两人 + 四个节日 + 一个生日。
    cfg->start.year = 2026;
    cfg->start.month = 8;
    cfg->start.day = 13;

    love_utf8_copy(cfg->people[0].name, sizeof(cfg->people[0].name), "咕咕");
    cfg->people[0].icon = LOVE_ICON_BIRD;
    love_utf8_copy(cfg->people[1].name, sizeof(cfg->people[1].name), "嘎嘎");
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
        love_utf8_copy(event->name, sizeof(event->name), DEFAULTS[i].name);
        event->icon = DEFAULTS[i].icon;
        event->kind = (uint8_t)DEFAULTS[i].kind;
        event->date.year = cfg->start.year;
        event->date.month = (int8_t)DEFAULTS[i].month;
        event->date.day = (int8_t)DEFAULTS[i].day;
        // 出厂默认进列表:一屏就能看到接下来几个日子,比一个事件一屏更适合新机。
        // (升级上来的老设备仍然保持原来的浏览方式,见 love_config.c 的迁移。)
        event->view_mode = LOVE_EVENT_VIEW_LIST;
    }
    cfg->event_count = (uint8_t)count;
    cfg->blank_off_seconds = LOVE_BLANK_OFF_DEFAULT;
    cfg->ble_enabled = 0;   // 出厂默认关
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

    // 先探长度再读:老记录(v2)比现在短,按固定长度读会对不上,而长度又决定怎么解释字节。
    // **中转缓冲走堆**:一份完整记录 v4 是 1460 字节,放栈上会把调用方的任务顶穿 ——
    // 串口控制台任务(4KB 栈)实测就是这样栈溢出的。一次读一次释放,不常驻。
    size_t size = 0;
    esp_err_t err = nvs_get_blob(handle, KEY_CONFIG, NULL, &size);
    if (err == ESP_OK && size > 0) {
        void *blob = malloc(size);
        if (blob) {
            err = nvs_get_blob(handle, KEY_CONFIG, blob, &size);
            if (err == ESP_OK && !love_config_from_record(blob, size, cfg)) {
                ESP_LOGW(TAG, "配置记录版本或长度不符(%u 字节),回落到默认值", (unsigned)size);
                love_config_defaults(cfg);
            }
            free(blob);
        } else {
            ESP_LOGW(TAG, "没有内存读配置记录(%u 字节),用默认值", (unsigned)size);
        }
    }
    nvs_close(handle);

    love_config_sanitize(cfg);
}

esp_err_t love_store_save_config(const love_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    // 同样是 1460 字节的一整份记录,同样不能放在调用方的栈上(保存路径里有按键任务
    // 与网页任务)。nvs_set_blob 会立刻把数据拷进 NVS,写完就能释放。
    love_config_record_t *record = malloc(sizeof(*record));
    if (!record) return ESP_ERR_NO_MEM;
    record->version = LOVE_CONFIG_VERSION;
    record->config = *cfg;
    love_config_sanitize(&record->config);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, KEY_CONFIG, record, sizeof(*record));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    free(record);
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

esp_err_t love_store_save_ap_off(bool off)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(handle, KEY_AP_OFF, off ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool love_store_load_ap_off(void)
{
    if (!s_ready) return false;

    nvs_handle_t handle;
    if (nvs_open(LOVE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    // 键不存在(老固件升上来)与写坏都是同一个意思:没有"手动关过"的意图。
    uint8_t value = 0;
    esp_err_t err = nvs_get_u8(handle, KEY_AP_OFF, &value);
    nvs_close(handle);
    return err == ESP_OK && value != 0;
}

esp_err_t love_store_load_ap_pass(char *out, size_t size)
{
    if (!out || size < 9) return ESP_ERR_INVALID_ARG;   // 最短 8 字符 + NUL
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    size_t len = size;
    err = nvs_get_str(handle, KEY_AP_PASS, out, &len);
    if (err == ESP_OK && out[0] != '\0') {
        nvs_close(handle);
        return ESP_OK;   // 老设备:沿用已经生成过的
    }

    // 首次(或键丢了):生成一个新的并落盘。字母表去掉了容易看错的 0/o/1/l/I,
    // 因为主人要从屏幕上把它念到手机里。
    static const char ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789";
    char generated[9];
    for (size_t i = 0; i + 1 < sizeof(generated); i++) {
        generated[i] = ALPHABET[esp_random() % (sizeof(ALPHABET) - 1)];
    }
    generated[sizeof(generated) - 1] = '\0';

    err = nvs_set_str(handle, KEY_AP_PASS, generated);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "热点密码落盘失败: %s", esp_err_to_name(err));
        return err;
    }
    snprintf(out, size, "%s", generated);
    return ESP_OK;
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
    // 来源是枚举而不是自由字段,NVS 里的值可能是被改坏或降级固件写进来的,
    // 超出已知范围就当作"未同步"。上界必须跟着 love_time_src_t 的最后一项目走。
    if (record.source > (uint8_t)LOVE_TIME_SRC_CONSOLE) record.source = (uint8_t)LOVE_TIME_SRC_NONE;
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

