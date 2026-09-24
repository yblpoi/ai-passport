// main/love_log.c —— 日志级别策略的实现(设计意图见 love_log.h)。
#include "love_log.h"

#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

// 出厂默认:两个 Wi-Fi 驱动 TAG 降到 warn。
//
// 实测的噪音源:没连上热点时,STA 每轮重连都会让 `wifi`/`wpa` 打一串状态行,而设备
// 自己的 love_net.log 只在转折点出声(每轮每条候选失败一行、整轮失败一行;间隔由候选
// 超时与退避决定,至少 30 秒一轮)—— 于是真正有用的那几条会被驱动日志埋掉
// (memory 里的原话:"在无 wifi 连接时,串口上不要打印那么多日志")。
// 需要排查连接问题时可以 `log wifi info` 临时打开。
static const love_log_override_t DEFAULTS[] = {
    { "wifi", ESP_LOG_WARN },
    { "wpa",  ESP_LOG_WARN },
};

static love_log_override_t s_overrides[LOVE_LOG_TAG_COUNT];
static size_t s_override_count;
static esp_log_level_t s_global;
static bool s_muted;
static bool s_inited;

// 把当前策略真正下发给 IDF(全局 + 每条覆盖)。
static void apply_all(void)
{
    if (s_muted) return;   // 静默窗口内不生效,unmute 时统一装回去

    // 先设全局:它会清掉 IDF 里所有按 TAG 的条目,所以必须在装覆盖之前做。
    esp_log_level_set("*", s_global);
    for (size_t i = 0; i < s_override_count; i++) {
        esp_log_level_set(s_overrides[i].tag, s_overrides[i].level);
    }
}

static bool level_supported(esp_log_level_t level)
{
    return level <= (esp_log_level_t)CONFIG_LOG_MAXIMUM_LEVEL;
}

esp_err_t love_log_init(void)
{
    if (s_inited) return ESP_OK;

    s_global = (esp_log_level_t)CONFIG_LOG_DEFAULT_LEVEL;
    s_override_count = 0;
    for (size_t i = 0; i < sizeof(DEFAULTS) / sizeof(DEFAULTS[0]); i++) {
        s_overrides[s_override_count++] = DEFAULTS[i];
    }

    apply_all();
    s_inited = true;
    // 自己打一条:看到这行就说明默认策略已经装好(否则用户会以为 log 命令没生效)。
    ESP_LOGI("love_log", "日志策略已装载:全局 %s,wifi/wpa 降为 warn",
             love_log_level_name(s_global));
    return ESP_OK;
}

esp_log_level_t love_log_global_level(void)
{
    return s_global;
}

esp_err_t love_log_set_global(esp_log_level_t level)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!level_supported(level)) return ESP_ERR_NOT_SUPPORTED;

    s_global = level;
    apply_all();
    return ESP_OK;
}

static love_log_override_t *find_override(const char *tag)
{
    for (size_t i = 0; i < s_override_count; i++) {
        if (strcmp(s_overrides[i].tag, tag) == 0) return &s_overrides[i];
    }
    return NULL;
}

esp_err_t love_log_set_tag(const char *tag, esp_log_level_t level)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!tag || tag[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!level_supported(level)) return ESP_ERR_NOT_SUPPORTED;
    // "*" 必须走全局那条路(get_global):IDF 里 set("*") 是"清空所有按 TAG 的条目并
    // 改默认级别"。若把它当普通覆盖收进表,apply_all() 会把它放在最后一条装上 ——
    // 此后任何 log <模块> <级别> 都压不住(整机静音),而 `log` 打印的表又与实际不符。
    if (strcmp(tag, "*") == 0 || strcmp(tag, "all") == 0) return ESP_ERR_INVALID_ARG;
    if (strlen(tag) >= LOVE_LOG_TAG_MAX) return ESP_ERR_INVALID_SIZE;

    love_log_override_t *slot = find_override(tag);
    if (!slot) {
        if (s_override_count >= LOVE_LOG_TAG_COUNT) return ESP_ERR_NO_MEM;
        slot = &s_overrides[s_override_count++];
        snprintf(slot->tag, sizeof(slot->tag), "%s", tag);
    }
    slot->level = level;
    if (!s_muted) esp_log_level_set(slot->tag, level);
    return ESP_OK;
}

size_t love_log_overrides(love_log_override_t *out, size_t max)
{
    if (!out || max == 0) return 0;
    const size_t n = s_override_count < max ? s_override_count : max;
    memcpy(out, s_overrides, n * sizeof(out[0]));
    return n;
}

void love_log_reset(void)
{
    if (!s_inited) return;

    s_global = (esp_log_level_t)CONFIG_LOG_DEFAULT_LEVEL;
    s_override_count = 0;
    for (size_t i = 0; i < sizeof(DEFAULTS) / sizeof(DEFAULTS[0]); i++) {
        s_overrides[s_override_count++] = DEFAULTS[i];
    }
    apply_all();
}

void love_log_mute_all(void)
{
    if (!s_inited || s_muted) return;

    s_muted = true;
    esp_log_level_set("*", ESP_LOG_NONE);
}

void love_log_unmute_all(void)
{
    if (!s_inited || !s_muted) return;

    s_muted = false;
    apply_all();
}

const char *love_log_level_name(esp_log_level_t level)
{
    switch (level) {
    case ESP_LOG_NONE:  return "none";
    case ESP_LOG_ERROR: return "error";
    case ESP_LOG_WARN:  return "warn";
    case ESP_LOG_INFO:  return "info";
    case ESP_LOG_DEBUG: return "debug";
    case ESP_LOG_VERBOSE: return "verbose";
    default: return "?";
    }
}

bool love_log_level_parse(const char *text, esp_log_level_t *out)
{
    if (!text || !out) return false;

    // off 是用户自然会敲的那个词,按 none 收下(两种写法在 help 里都写出来)。
    if (strcmp(text, "none") == 0 || strcmp(text, "off") == 0) *out = ESP_LOG_NONE;
    else if (strcmp(text, "error") == 0) *out = ESP_LOG_ERROR;
    else if (strcmp(text, "warn") == 0) *out = ESP_LOG_WARN;
    else if (strcmp(text, "info") == 0) *out = ESP_LOG_INFO;
    else if (strcmp(text, "debug") == 0) *out = ESP_LOG_DEBUG;
    else if (strcmp(text, "verbose") == 0) *out = ESP_LOG_VERBOSE;
    else return false;
    return true;
}
