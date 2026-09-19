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
// 老固件的单条凭据。新固件只用它做一次搬迁,migrate_legacy_wifi 之后就会被擦掉。
#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PASS "wifi_pass"
#define KEY_WIFI_LIST "wifi_list"   // love_wifi_cred_t 数组
#define KEY_WIFI_COUNT "wifi_n"
#define KEY_AP_OFF "ap_off"
#define KEY_AP_PASS "ap_pass"
#define KEY_DEBUG "debug"
#define KEY_TIME "time"
#define KEY_DAYS_CACHE "days"
#define KEY_AVATAR_PREFIX "av"    // NVS key 上限 15 字节:av0..av3
// 头像自带的 16 色调色板(ap0..ap3)。与 av0..av3 分开存,见 love_config.h 的说明。
#define KEY_AVATAR_PALETTE_PREFIX "ap"

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

// NVS 里那份 blob 就是结构体的原始字节,所以两个定长数组之间**不能有填充**:
// 布局一变,老设备读回来就是错位的凭据。钉住它,并把总数作为注释留在这里。
_Static_assert(sizeof(love_wifi_cred_t) == LOVE_WIFI_SSID_MAX + LOVE_WIFI_PASS_MAX,
               "love_wifi_cred_t 必须是紧凑布局(98 字节),它就是 NVS blob 的格式");

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

    // 默认给四个固定的公历节日 + 生日,再补三个农历节日(春节/中秋/七夕),
    // 让"农历"这个能力一开机就能看见,不用用户自己去配。
    const struct {
        const char *name;
        uint8_t icon;
        love_event_kind_t kind;
        int month;
        int day;
    } DEFAULTS[] = {
        { "元旦",      LOVE_ICON_GIFT,        LOVE_EVENT_YEARLY, 1,  1 },
        { "情人节",    LOVE_ICON_HEART,       LOVE_EVENT_YEARLY, 2,  14 },
        { "咕咕嘎嘎",  LOVE_ICON_CAKE,        LOVE_EVENT_YEARLY, 7,  15 },
        { "国庆节",    LOVE_ICON_STAR,        LOVE_EVENT_YEARLY, 10, 1 },
        { "圣诞节",    LOVE_ICON_TREE,        LOVE_EVENT_YEARLY, 12, 25 },
        { "春节",      LOVE_ICON_FIRECRACKER, LOVE_EVENT_LUNAR,  1,  1 },
        { "中秋",      LOVE_ICON_RABBIT,      LOVE_EVENT_LUNAR,  8,  15 },
        { "七夕",      LOVE_ICON_BOUQUET,     LOVE_EVENT_LUNAR,  7,  7 },
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

    // 这里**不**预读一次配置:此刻 s_ready 还没置位,love_store_load_config() 会在
    // 铺完默认值后立刻返回(既不读 NVS 也没有任何落盘),唯一的后果是在栈上摆一份
    // 1454 字节的配置再整个丢掉 —— 而本函数跑在只有 3584 字节栈的 main 任务上。
    // 真正需要配置的调用方(love_app)紧接着会自己 load 一份到它的状态里。
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

esp_err_t love_store_save_wifi_list(const love_wifi_cred_t *list, size_t count)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!list || count == 0 || count > LOVE_WIFI_MAX) return ESP_ERR_INVALID_ARG;

    for (size_t i = 0; i < count; i++) {
        // 定长数组必须是 NUL 结尾的:strnlen 到不了结尾就说明这份数据是坏的,
        // 宁可整批拒绝,也不要写进一条后面没有结尾的 SSID(NVS 读回来会越界)。
        const size_t ssid_len = strnlen(list[i].ssid, LOVE_WIFI_SSID_MAX);
        const size_t pass_len = strnlen(list[i].pass, LOVE_WIFI_PASS_MAX);
        if (ssid_len == 0 || ssid_len >= LOVE_WIFI_SSID_MAX ||
            pass_len >= LOVE_WIFI_PASS_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(handle, KEY_WIFI_COUNT, (uint8_t)count);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, KEY_WIFI_LIST, list, count * sizeof(list[0]));
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    // 不在日志里输出 SSID/密码。
    if (err != ESP_OK) ESP_LOGE(TAG, "保存 Wi-Fi 列表失败: %s", esp_err_to_name(err));
    return err;
}

// 老固件的单条记录 -> 列表。返回 ESP_OK 表示 out/count 里已经是搬迁后的结果。
static esp_err_t migrate_legacy_wifi(love_wifi_cred_t *out, size_t max, size_t *count)
{
    if (!out || !count || max == 0) return ESP_ERR_INVALID_ARG;
    *count = 0;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    char ssid[LOVE_WIFI_SSID_MAX] = { 0 };
    size_t ssid_size = sizeof(ssid);
    err = nvs_get_str(handle, KEY_WIFI_SSID, ssid, &ssid_size);
    if (err == ESP_OK && ssid[0] != '\0') {
        char pass[LOVE_WIFI_PASS_MAX] = { 0 };
        size_t pass_size = sizeof(pass);
        // 开放网络没有密码这一项,读不到就当空密码 —— 那是合法状态,不是错误。
        if (nvs_get_str(handle, KEY_WIFI_PASS, pass, &pass_size) != ESP_OK) pass[0] = '\0';

        snprintf(out[0].ssid, sizeof(out[0].ssid), "%s", ssid);
        snprintf(out[0].pass, sizeof(out[0].pass), "%s", pass);
        *count = 1;

        // 先写新格式、成功了再擦旧键。反过来一旦写到一半失败,凭据就真没了 ——
        // 而这里两条旧键还在的唯一后果,只是下次开机再搬一遍。
        err = nvs_set_u8(handle, KEY_WIFI_COUNT, (uint8_t)*count);
        if (err == ESP_OK) {
            err = nvs_set_blob(handle, KEY_WIFI_LIST, out, *count * sizeof(out[0]));
        }
        if (err == ESP_OK) {
            (void)nvs_erase_key(handle, KEY_WIFI_SSID);   // 不存在时返回 NOT_FOUND,忽略
            (void)nvs_erase_key(handle, KEY_WIFI_PASS);
            err = nvs_commit(handle);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi 凭据搬迁未完成(%s),下次开机再试", esp_err_to_name(err));
            *count = 0;
        } else {
            ESP_LOGI(TAG, "Wi-Fi 凭据已迁移到列表格式(1 个热点)");
        }
    }
    nvs_close(handle);
    return *count > 0 ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

size_t love_store_load_wifi_list(love_wifi_cred_t *out, size_t max)
{
    if (!s_ready || !out || max == 0) return 0;
    memset(out, 0, max * sizeof(out[0]));

    nvs_handle_t handle;
    if (nvs_open(LOVE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0;

    uint8_t count = 0;
    size_t size = max * sizeof(out[0]);
    esp_err_t err = nvs_get_u8(handle, KEY_WIFI_COUNT, &count);
    if (err == ESP_OK) err = nvs_get_blob(handle, KEY_WIFI_LIST, out, &size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        size_t migrated = 0;
        if (migrate_legacy_wifi(out, max, &migrated) != ESP_OK) return 0;
        return migrated;
    }
    if (err != ESP_OK) return 0;

    // 记录本身是"计数 + blob"两条,理论上不会半新半旧。真读坏了(计数大于 blob、
    // 或者中间出现空 SSID)就截断到仍然可信的那一段,而不是把整份有效凭据丢掉。
    size_t stored = size / sizeof(out[0]);
    if (stored < count) count = (uint8_t)stored;
    if (count > LOVE_WIFI_MAX) count = LOVE_WIFI_MAX;
    size_t usable = 0;
    for (size_t i = 0; i < count; i++) {
        // 结尾强制补 NUL:blob 里的字符串没有"一定结尾"的保证,后面 strlen 会越界。
        out[i].ssid[LOVE_WIFI_SSID_MAX - 1] = '\0';
        out[i].pass[LOVE_WIFI_PASS_MAX - 1] = '\0';
        if (out[i].ssid[0] == '\0') break;
        usable++;
    }
    return usable;
}

esp_err_t love_store_clear_wifi(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    (void)nvs_erase_key(handle, KEY_WIFI_LIST);
    (void)nvs_erase_key(handle, KEY_WIFI_COUNT);
    (void)nvs_erase_key(handle, KEY_WIFI_SSID);
    (void)nvs_erase_key(handle, KEY_WIFI_PASS);
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

/* ---------- NVS 样板 ---------- */
// 下面这些接口除键名与记录类型之外完全同形,"开句柄 → 读/写 → 提交 → 关句柄"各抄
// 一遍的话,迟早会漏掉其中一处(漏 commit 是静默丢数据,漏 close 是漏资源)。
// 传输缓冲一律由调用方提供,这两层自己不持有任何状态,也就不需要加锁。

static esp_err_t store_set_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

// 读一个 u8 键。键不存在(老固件升上来)、写坏,都由调用方按"没写过"处理。
static bool store_get_u8(const char *key, uint8_t *out)
{
    nvs_handle_t handle;
    if (nvs_open(LOVE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    uint8_t value = 0;
    const esp_err_t err = nvs_get_u8(handle, key, &value);
    nvs_close(handle);
    if (err != ESP_OK) return false;

    *out = value;
    return true;
}

static esp_err_t store_set_blob(const char *key, const void *data, size_t size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, key, data, size);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

// 读一个 blob。长度与预期不符就整体判为无效:每条记录都带版本号,而长度同样是契约的
// 一部分(短读或降级固件写进来的残片都不该被当成本版本的记录解释)。
static bool store_get_blob(const char *key, void *out, size_t size)
{
    nvs_handle_t handle;
    if (nvs_open(LOVE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    size_t len = size;
    const esp_err_t err = nvs_get_blob(handle, key, out, &len);
    nvs_close(handle);
    return err == ESP_OK && len == size;
}

esp_err_t love_store_save_ap_off(bool off)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return store_set_u8(KEY_AP_OFF, off ? 1 : 0);
}

bool love_store_load_ap_off(void)
{
    if (!s_ready) return false;

    // 键不存在(老固件升上来)与写坏都是同一个意思:没有"手动关过"的意图。
    uint8_t value = 0;
    return store_get_u8(KEY_AP_OFF, &value) && value != 0;
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

esp_err_t love_store_save_debug_mode(bool on)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return store_set_u8(KEY_DEBUG, on ? 1 : 0);
}

bool love_store_load_debug_mode(void)
{
    if (!s_ready) return false;

    uint8_t value = 0;
    return store_get_u8(KEY_DEBUG, &value) && value != 0;
}

esp_err_t love_store_save_time(uint64_t epoch_seconds, love_time_src_t src)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    const time_record_t record = { epoch_seconds, (uint8_t)src };
    return store_set_blob(KEY_TIME, &record, sizeof(record));
}

bool love_store_load_time(uint64_t *epoch_seconds, love_time_src_t *src)
{
    if (epoch_seconds) *epoch_seconds = 0;
    if (src) *src = LOVE_TIME_SRC_NONE;
    if (!s_ready) return false;

    time_record_t record = { 0, 0 };
    if (!store_get_blob(KEY_TIME, &record, sizeof(record))) return false;
    if (record.epoch_seconds == 0) return false;
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
    return store_set_blob(KEY_DAYS_CACHE, &record, sizeof(record));
}

bool love_store_load_days_cache(int32_t *days, uint64_t *epoch_seconds)
{
    if (days) *days = 0;
    if (epoch_seconds) *epoch_seconds = 0;
    if (!s_ready) return false;

    days_cache_t record = { 0, 0, 0 };
    if (!store_get_blob(KEY_DAYS_CACHE, &record, sizeof(record))) return false;
    if (record.version != DAYS_CACHE_VERSION) return false;
    if (record.epoch_seconds == 0) return false;

    if (days) *days = record.days;
    if (epoch_seconds) *epoch_seconds = record.epoch_seconds;
    return true;
}

/* ---------- 自定义头像 ---------- */

static void avatar_key(const char *prefix, uint8_t slot, char *out, size_t size)
{
    snprintf(out, size, "%s%u", prefix, (unsigned)slot);
}

esp_err_t love_store_save_avatar(uint8_t slot, const void *data)
{
    if (slot >= LOVE_AVATAR_MAX || !data) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    avatar_record_t record = { .version = AVATAR_VERSION };
    memcpy(record.data, data, LOVE_AVATAR_BYTES);

    char key[8];
    char pal_key[8];
    avatar_key(KEY_AVATAR_PREFIX, slot, key, sizeof(key));
    avatar_key(KEY_AVATAR_PALETTE_PREFIX, slot, pal_key, sizeof(pal_key));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    // 先让新索引与旧配色脱钩:网页要是接着传配色,下一次调用会把它写回来;
    // 只传 800 字节索引的上传到此结束 —— 语义正好是"回到设备那 16 色图标配色"。
    err = nvs_set_blob(handle, key, &record, sizeof(record));
    if (err == ESP_OK) (void)nvs_erase_key(handle, pal_key);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) ESP_LOGE(TAG, "保存头像 %u 失败: %s", (unsigned)slot, esp_err_to_name(err));
    return err;
}

esp_err_t love_store_save_avatar_palette(uint8_t slot, const void *palette)
{
    if (slot >= LOVE_AVATAR_MAX || !palette) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    char key[8];
    avatar_key(KEY_AVATAR_PALETTE_PREFIX, slot, key, sizeof(key));

    const esp_err_t err = store_set_blob(key, palette, LOVE_AVATAR_PALETTE_BYTES);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存头像配色 %u 失败: %s", (unsigned)slot, esp_err_to_name(err));
    }
    return err;
}

size_t love_store_load_avatar_palette(uint8_t slot, void *out, size_t out_size)
{
    if (slot >= LOVE_AVATAR_MAX || !out || out_size < LOVE_AVATAR_PALETTE_BYTES) return 0;
    if (!s_ready) return 0;

    char key[8];
    avatar_key(KEY_AVATAR_PALETTE_PREFIX, slot, key, sizeof(key));

    // 没有这个键 = 这张头像用设备那 16 色图标配色(老固件上传的、或网页只传了 800 字节)。
    if (!store_get_blob(key, out, LOVE_AVATAR_PALETTE_BYTES)) return 0;
    return LOVE_AVATAR_PALETTE_BYTES;
}

size_t love_store_load_avatar(uint8_t slot, void *out, size_t out_size)
{
    if (slot >= LOVE_AVATAR_MAX || !out || out_size < LOVE_AVATAR_BYTES) return 0;
    if (!s_ready) return 0;

    char key[8];
    avatar_key(KEY_AVATAR_PREFIX, slot, key, sizeof(key));

    avatar_record_t record;
    if (!store_get_blob(key, &record, sizeof(record))) return 0;
    if (record.version != AVATAR_VERSION) return 0;

    memcpy(out, record.data, LOVE_AVATAR_BYTES);
    return LOVE_AVATAR_BYTES;
}

esp_err_t love_store_clear_avatar(uint8_t slot)
{
    if (slot >= LOVE_AVATAR_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    // 头像与它的调色板一起清:留着 apN 会变成"没有头像却有配色"的残片,
    // 下次上传要是只传 800 字节就会被这份残片染色。
    char key[8];
    char pal_key[8];
    avatar_key(KEY_AVATAR_PREFIX, slot, key, sizeof(key));
    avatar_key(KEY_AVATAR_PALETTE_PREFIX, slot, pal_key, sizeof(pal_key));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LOVE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    (void)nvs_erase_key(handle, key);
    (void)nvs_erase_key(handle, pal_key);
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

