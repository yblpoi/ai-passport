// main/power_sleep.c —— 浅睡眠 / 深睡眠实现,不含任何界面代码。
//
// 两种模式入睡前均 suspend ES8311;light sleep 返回后显式恢复。
// deep sleep 还会按 CW2017 -> ES8311 -> I2S -> 共享 I2C -> LCD 顺序停止外设。
// 这条顺序被 tests/test_deep_sleep_contract.py 逐项断言,改动前先看那个测试。
#include "power_sleep.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_i2c.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power_sleep";

#define LIGHT_SLEEP_TIME_US ((uint64_t)POWER_SLEEP_LIGHT_SECONDS * 1000ULL * 1000ULL)
#define DEEP_SLEEP_TIME_US  ((uint64_t)POWER_SLEEP_DEEP_SECONDS * 1000ULL * 1000ULL)
#define DEEP_SLEEP_MAGIC    0x464F4C4FUL
#define SLEEP_WORKER_STACK 3072

typedef enum {
    SLEEP_COMMAND_LIGHT = 1,
    SLEEP_COMMAND_DEEP,
} sleep_command_t;

// 工作线程随应用存活:本固件没有退出流程,深睡眠成功也是直接重启,
// 所以不需要停止路径,只做懒创建。
static TaskHandle_t s_task;
static volatile bool s_busy;
static portMUX_TYPE s_result_lock = portMUX_INITIALIZER_UNLOCKED;
static power_sleep_result_t s_result;
static RTC_DATA_ATTR uint32_t s_deep_sleep_magic;
static RTC_DATA_ATTR uint32_t s_deep_sleep_count;

static void publish_result(const power_sleep_result_t *result)
{
    taskENTER_CRITICAL(&s_result_lock);
    s_result = *result;
    taskEXIT_CRITICAL(&s_result_lock);
}

static void log_deep_sleep_warning(const char *step, esp_err_t error)
{
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep 继续：%s 失败: %s", step,
                 esp_err_to_name(error));
    }
}

static void run_light_sleep(void)
{
    const char *failure = "Light sleep";
    bool audio_suspend_attempted = false;
    power_sleep_result_t result = { .attempted = true };

    vTaskDelay(pdMS_TO_TICKS(150));

    esp_err_t err = esp_sleep_enable_timer_wakeup(LIGHT_SLEEP_TIME_US);
    if (err == ESP_OK) {
        audio_suspend_attempted = true;
        err = bsp_audio_sleep();
        failure = "Audio suspend";
    }
    if (err == ESP_OK) bsp_display_backlight(0);
    int64_t before = esp_timer_get_time();
    if (err == ESP_OK) {
        failure = "Light sleep";
        err = esp_light_sleep_start();
    }
    result.slept_ms = (int32_t)((esp_timer_get_time() - before) / 1000);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    // bsp_audio_sleep() 可能在已停 I2S 后报寄存器校验失败,
    // 因此只要尝试过 suspend,未进入 light sleep 也必须恢复。
    if (audio_suspend_attempted) {
        esp_err_t wake_err = bsp_audio_wake();
        if (err == ESP_OK && wake_err != ESP_OK) {
            err = wake_err;
            failure = "Audio resume";
        }
    }
    bsp_display_backlight(100);

    result.ok = (err == ESP_OK);
    result.error = err;
    publish_result(&result);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "浅睡眠唤醒: RTC 定时器, 实际睡了 %d ms", (int)result.slept_ms);
    } else {
        ESP_LOGE(TAG, "%s 失败: %s", failure, esp_err_to_name(err));
    }
}

static void run_deep_sleep(void)
{
    power_sleep_result_t result = { .attempted = true, .deep = true };

    vTaskDelay(pdMS_TO_TICKS(250));

    esp_err_t err = esp_sleep_enable_timer_wakeup(DEEP_SLEEP_TIME_US);
    if (err != ESP_OK) {
        result.error = err;
        publish_result(&result);
        ESP_LOGE(TAG, "Deep sleep 失败: %s", esp_err_to_name(err));
        return;
    }

    // CW2017 与 ES8311 共用 I2C，必须先完成电量计写入/回读。
    log_deep_sleep_warning("CW2017 suspend", bsp_battery_sleep());
    log_deep_sleep_warning("ES8311 suspend", bsp_audio_sleep());
    // 即使 codec 寄存器操作失败，也继续停时钟并释放引脚。
    log_deep_sleep_warning("I2S pin release", bsp_audio_prepare_deep_sleep());
    log_deep_sleep_warning("shared I2C pin release", bsp_i2c_prepare_deep_sleep());

    // Wi-Fi/BLE/HTTP 由调用方在请求深睡眠前停止并释放。
    // 加锁等待当前 flush 完成，然后阻止 LVGL 在 LCD 关闭后再刷屏。
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "deep sleep 前无法停止 LVGL 刷屏，重启恢复外设");
        esp_restart();
    }
    log_deep_sleep_warning("ST7789 suspend", bsp_display_prepare_deep_sleep());

    if (s_deep_sleep_magic != DEEP_SLEEP_MAGIC) s_deep_sleep_count = 0;
    s_deep_sleep_magic = DEEP_SLEEP_MAGIC;
    s_deep_sleep_count++;
    esp_deep_sleep_start();
    // 从 deep-sleep 准备接口返回后总线已不可在本次运行中恢复。
    ESP_LOGE(TAG, "esp_deep_sleep_start 意外返回，重启恢复外设");
    esp_restart();
}

static void sleep_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t command = 0;
        xTaskNotifyWait(0, UINT32_MAX, &command, portMAX_DELAY);

        s_busy = true;
        if (command == SLEEP_COMMAND_DEEP) {
            run_deep_sleep();
        } else {
            run_light_sleep();
        }
        s_busy = false;
    }
}

static esp_err_t ensure_worker(void)
{
    if (s_task) return ESP_OK;
    if (xTaskCreate(sleep_task, "power_sleep", SLEEP_WORKER_STACK, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "创建休眠工作线程失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t request(uint32_t command)
{
    esp_err_t err = ensure_worker();
    if (err != ESP_OK) return err;
    s_busy = true;
    xTaskNotify(s_task, command, eSetValueWithOverwrite);
    return ESP_OK;
}

esp_err_t power_sleep_light(void)
{
    if (s_busy) return ESP_ERR_INVALID_STATE;
    return request(SLEEP_COMMAND_LIGHT);
}

esp_err_t power_sleep_deep(void)
{
    if (s_busy) return ESP_ERR_INVALID_STATE;
    return request(SLEEP_COMMAND_DEEP);
}

bool power_sleep_busy(void)
{
    return s_busy;
}

void power_sleep_get_result(power_sleep_result_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_result_lock);
    *out = s_result;
    taskEXIT_CRITICAL(&s_result_lock);
}

uint32_t power_sleep_deep_count(void)
{
    return s_deep_sleep_magic == DEEP_SLEEP_MAGIC ? s_deep_sleep_count : 0;
}

bool power_sleep_woke_from_deep(void)
{
    return s_deep_sleep_magic == DEEP_SLEEP_MAGIC &&
           esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
}
