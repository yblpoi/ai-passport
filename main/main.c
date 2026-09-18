// main/main.c —— 启动流程与按键分发。
//
// 按键语义全部由宿主应用(像素风纪念日摆件)解释:
//   上/下 短按   主屏进轮播、翻页(单页卡与列表页共用一套页码);设置页移动选中项
//   确定  短按   设置页/状态页执行动作;主屏与轮播上不做事
//   确定  长按   进入设置页;设置页里返回主屏
//
// 注意 BSP 会给**一次**物理按键发好几个事件(按下 PRESS,松开后才是组件判定的
// CLICK / DOUBLE / LONG),哪些算"用户想做的事"由 main/love_key.h 统一裁定 ——
// 按下不算动作(否则熄屏时按下亮屏、判定事件又会紧接着执行一遍)。
//
// 开机直接进入应用,没有中间菜单:外设的可用性只记日志。应用自身会按需降级
// (取不到电量显示 "--",没对过时显示 "未同步"),所以不需要一张 [FAIL] 状态表。
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "love_app.h"
#include "power_sleep.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";



#define INPUT_QUEUE_DEPTH 8
#define INPUT_TASK_STACK  4096

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;

static void input_task(void *arg)
{
    (void)arg;
    input_event_t input;
    for (;;) {
        // **不要**改成 portMAX_DELAY:无限阻塞的任务会被 FreeRTOS 从所有链表里摘掉,
        // 于是 xTaskGetHandle("input") 永远找不到它 —— 而串口 `status` 正是靠这个
        // 打印各任务的栈余,这条"机身按键 → 整屏重绘"的路恰恰是最该盯着的一条。
        // 一秒一次空转的代价可以忽略,顺带还是空闲检查(深睡眠)的心跳。
        if (xQueueReceive(s_input_queue, &input, pdMS_TO_TICKS(1000)) == pdTRUE) {
            love_app_key(input.btn, input.event);
            continue;
        }
        // 熄屏后长时间无人操作就睡下去。放在本任务上做,因为入睡要停 BLE/HTTP/Wi-Fi,
        // 而 love_ble_stop() 会无超时等 NimBLE host 任务退出 —— 不能在 LVGL 任务上做。
        love_app_idle_poll();
    }
}

// button callbacks run on the shared esp_timer task; enqueue only and return immediately.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static esp_err_t input_dispatch_init(void)
{
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "input", INPUT_TASK_STACK, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "FoloToy AI Passport 启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }
    // 抄一份唤醒原因进 RTC 内存:主机开一次串口就可能复位芯片,复位后实时寄存器
    // 就变"上电/复位"了(见 power_sleep.h)。
    power_sleep_note_boot();

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是唯一界面,初始化失败就没有可用的产品形态 —— 打清楚日志后退出,
    // 不做"串口降级"(那会让本文件复杂一倍,而这条路径无法被真机验证)。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 其余外设单项失败不阻塞:应用会降级显示,而不是拿不到数据就拒绝启动。
    esp_err_t input_err = input_dispatch_init();
    esp_err_t button_err = input_err == ESP_OK
                         ? bsp_button_init(on_key, NULL)
                         : ESP_ERR_INVALID_STATE;
    if (input_err != ESP_OK) {
        ESP_LOGE(TAG, "按键事件任务创建失败: %s", esp_err_to_name(input_err));
    } else if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(button_err));
    }
    esp_err_t audio_err = bsp_audio_init();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "音频初始化失败: %s", esp_err_to_name(audio_err));
    }
    esp_err_t battery_err = bsp_battery_init();
    if (battery_err != ESP_OK) {
        ESP_LOGW(TAG, "电量计初始化失败: %s", esp_err_to_name(battery_err));
    }
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL 上锁失败,无法进入应用");
        return;
    }
    love_app_enter();
    bsp_lvgl_unlock();

    // 按键是唯一的输入:不可用时界面仍会渲染,只是不响应 —— 明确记一条日志,
    // 免得现场看到"屏亮但不理人"却找不到原因。
    if (button_err == ESP_OK) {
        s_input_ready = true;
    } else {
        ESP_LOGE(TAG, "按键不可用,界面只显示、不响应");
    }

    esp_err_t start_err = love_app_start();
    if (start_err != ESP_OK) {
        ESP_LOGW(TAG, "应用服务启动失败: %s", esp_err_to_name(start_err));
    }

    ESP_LOGI(TAG, "就绪:Display=1 Button=%d Audio=%d Battery=%d",
             button_err == ESP_OK, audio_err == ESP_OK, battery_err == ESP_OK);
}
