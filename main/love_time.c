// main/love_time.c —— 时间服务实现(无 RTC:基准时间 + 开机后微秒计数)。
#include "love_time.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <time.h>

static const char *TAG = "love_time";

// 早于 2020 或晚于 2100 的时间戳按无效处理,避免脏数据把界面带偏。
#define EPOCH_MIN 1577836800ull
#define EPOCH_MAX 4102444800ull

#define SNTP_ATTEMPT_TIMEOUT_MS 1000
#define SNTP_RETRY_INTERVAL_US  (60 * 1000000LL)        // 没同步上:每分钟再试
#define SNTP_RESYNC_INTERVAL_US (30 * 60 * 1000000LL)   // 已同步:每 30 分钟校准一次
#define SNTP_DRIFT_TOLERANCE_S  300                     // 偏差超过 5 分钟才重写
#define TIME_WORKER_STACK 4096
#define TIME_LISTENER_MAX 4

typedef enum {
    MSG_SET = 0,
    MSG_SNTP_START,
    MSG_SNTP_STOP,
} msg_kind_t;

typedef struct {
    msg_kind_t kind;
    uint64_t epoch_seconds;
    love_time_src_t source;
} time_msg_t;

typedef struct {
    love_time_listener_t fn;
    void *ctx;
} listener_t;

static QueueHandle_t s_queue;
static listener_t s_listeners[TIME_LISTENER_MAX];
static size_t s_listener_count;

// 基准:某个已知时刻 + 当时的开机微秒计数。
static uint64_t s_base_epoch;
static int64_t s_base_us;
static bool s_holds;
static love_time_src_t s_source = LOVE_TIME_SRC_NONE;

static bool s_sntp_enabled;
static bool s_sntp_inited;
static int64_t s_next_attempt_us;

static uint64_t mono_seconds_now(void)
{
    return (uint64_t)((esp_timer_get_time() - s_base_us) / 1000000);
}

// 当前推算时间(未对时时为 0)。
static uint64_t current_epoch(void)
{
    if (!s_holds) return 0;
    return s_base_epoch + mono_seconds_now();
}

static void notify_listeners(void)
{
    for (size_t i = 0; i < s_listener_count; i++) {
        if (s_listeners[i].fn) s_listeners[i].fn(s_listeners[i].ctx);
    }
}

static void apply_time(uint64_t epoch_seconds, love_time_src_t source)
{
    if (epoch_seconds < EPOCH_MIN || epoch_seconds > EPOCH_MAX) {
        ESP_LOGW(TAG, "忽略越界的时间戳");
        return;
    }

    // 同一秒内的重复写入(例如 SNTP 连续回调)不再写 NVS。
    if (s_holds && epoch_seconds == current_epoch()) {
        return;
    }

    s_base_epoch = epoch_seconds;
    s_base_us = esp_timer_get_time();
    s_holds = true;
    s_source = source;
    s_next_attempt_us = 0;

    esp_err_t err = love_store_save_time(epoch_seconds, source);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "时间未能写入 NVS: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "时间已更新(来源 %d)", (int)source);
    notify_listeners();
}

// 联网后由工作任务的循环调用:没同步上就重试,同步上了则定期校准,
// 因为设备没有 RTC,长时间运行只能靠网络时间拉回来。
static void sntp_try(void)
{
    if (!s_sntp_inited) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
            2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "cn.pool.ntp.org"));
        cfg.start = false;
        cfg.smooth_sync = false;
        esp_err_t err = esp_netif_sntp_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SNTP 初始化失败: %s", esp_err_to_name(err));
            s_next_attempt_us = esp_timer_get_time() + SNTP_RETRY_INTERVAL_US;
            return;
        }
        s_sntp_inited = true;
        esp_netif_sntp_start();
    }

    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_ATTEMPT_TIMEOUT_MS));
    if (err != ESP_OK) {
        s_next_attempt_us = esp_timer_get_time() + SNTP_RETRY_INTERVAL_US;
        return;
    }

    s_next_attempt_us = esp_timer_get_time() + SNTP_RESYNC_INTERVAL_US;

    time_t now = time(NULL);
    if (now <= 0) return;

    uint64_t epoch = (uint64_t)now;
    uint64_t current = current_epoch();
    int64_t drift = (int64_t)epoch - (int64_t)current;
    if (s_holds && drift < SNTP_DRIFT_TOLERANCE_S && drift > -SNTP_DRIFT_TOLERANCE_S) {
        return;   // 偏差很小,不必写 NVS
    }
    ESP_LOGI(TAG, "SNTP 对时成功");
    apply_time(epoch, LOVE_TIME_SRC_SNTP);
}

static void time_worker(void *arg)
{
    (void)arg;
    for (;;) {
        time_msg_t msg;
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(200)) == pdTRUE) {
            switch (msg.kind) {
            case MSG_SET:
                apply_time(msg.epoch_seconds, msg.source);
                break;
            case MSG_SNTP_START:
                s_sntp_enabled = true;
                s_next_attempt_us = 0;
                break;
            case MSG_SNTP_STOP:
                s_sntp_enabled = false;
                if (s_sntp_inited) {
                    esp_netif_sntp_deinit();
                    s_sntp_inited = false;
                }
                break;
            }
        }

        if (s_sntp_enabled && esp_timer_get_time() >= s_next_attempt_us) {
            sntp_try();
        }
    }
}

static esp_err_t send_msg(const time_msg_t *msg)
{
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_queue, msg, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t love_time_init(void)
{
    if (s_queue) return ESP_OK;

    s_queue = xQueueCreate(8, sizeof(time_msg_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(time_worker, "love_time", TIME_WORKER_STACK, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    uint64_t epoch = 0;
    love_time_src_t src = LOVE_TIME_SRC_NONE;
    if (love_store_load_time(&epoch, &src)) {
        s_base_epoch = epoch;
        s_base_us = esp_timer_get_time();
        s_holds = true;
        s_source = src;
        ESP_LOGI(TAG, "沿用上次对时结果(来源 %d)", (int)src);
    } else {
        ESP_LOGW(TAG, "尚无对时记录,界面会显示“时间未同步”");
    }
    return ESP_OK;
}

void love_time_get(love_time_state_t *state)
{
    if (!state) return;
    state->holds = s_holds;
    state->source = s_source;
    state->epoch_seconds = current_epoch();
    state->wifi_pending = s_sntp_enabled && !s_holds;
}

esp_err_t love_time_set(uint64_t epoch_seconds, love_time_src_t source)
{
    const time_msg_t msg = { MSG_SET, epoch_seconds, source };
    return send_msg(&msg);
}

esp_err_t love_time_sntp_start(void)
{
    const time_msg_t msg = { MSG_SNTP_START, 0, LOVE_TIME_SRC_NONE };
    return send_msg(&msg);
}

void love_time_sntp_stop(void)
{
    const time_msg_t msg = { MSG_SNTP_STOP, 0, LOVE_TIME_SRC_NONE };
    (void)send_msg(&msg);
}

const char *love_time_src_text(love_time_src_t source)
{
    switch (source) {
    case LOVE_TIME_SRC_SNTP: return "网络对时";
    case LOVE_TIME_SRC_WEB:  return "网页对时";
    case LOVE_TIME_SRC_BLE:  return "蓝牙对时";
    default:                 return "未同步";
    }
}

void love_time_describe(const love_time_state_t *state, char *buf, size_t size)
{
    if (!buf || size == 0) return;
    if (!state || !state->holds) {
        snprintf(buf, size, "时间未同步");
        return;
    }

    const char *source = love_time_src_text(state->source);

    love_date_t today = love_date_from_epoch(state->epoch_seconds, LOVE_TZ_OFFSET_SECONDS);
    int hour = 0, minute = 0;
    love_hms_from_epoch(state->epoch_seconds, LOVE_TZ_OFFSET_SECONDS, &hour, &minute, NULL);
    snprintf(buf, size, "%02d-%02d %02d:%02d %s",
             (int)today.month, (int)today.day, hour, minute, source);
}

esp_err_t love_time_add_listener(love_time_listener_t listener, void *ctx)
{
    if (!listener || s_listener_count >= TIME_LISTENER_MAX) return ESP_ERR_NO_MEM;
    s_listeners[s_listener_count].fn = listener;
    s_listeners[s_listener_count].ctx = ctx;
    s_listener_count++;
    return ESP_OK;
}
