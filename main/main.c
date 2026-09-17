// main/main.c —— FoloToy AI Passport BSP 驱动参考示例:初始化 + 菜单 + 按键分发。
//
// 按键语义(全局统一):
//   上/下 短按   菜单中=移动选中项;演示页中=该页自定义
//   确定  短按   菜单中=进入选中项;演示页中=该页自定义
//   确定  长按   演示页中=返回菜单(由本文件统一拦截)
//
// 开机默认进入宿主应用(像素风恋爱倒计时),长按确定键可在应用内进入设置页,
// 设置页里可以跳回下面的 demo 菜单;此时按键交给应用自己解释。
#include "app_shell.h"
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "demo.h"
#include "demo_navigation.h"
#include "love_app.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

static const demo_entry_t DEMOS[] = {
    { .name = "Countdown", .enter = love_app_enter, .exit = love_app_exit,
      .key = love_app_key, .start = love_app_start, .stop = love_app_stop },
    { .name = "Display", .enter = demo_display_enter, .exit = demo_display_exit,
      .key = demo_display_key },
    { .name = "Button", .enter = demo_button_enter, .exit = demo_button_exit,
      .key = demo_button_key },
    { .name = "Audio", .enter = demo_audio_enter, .exit = demo_audio_exit,
      .key = demo_audio_key, .start = demo_audio_start, .stop = demo_audio_stop },
    { .name = "Battery", .enter = demo_battery_enter, .exit = demo_battery_exit,
      .key = demo_battery_key },
    { .name = "Wi-Fi", .enter = demo_wifi_enter, .exit = demo_wifi_exit,
      .key = demo_wifi_key, .start = demo_wifi_start, .stop = demo_wifi_stop },
    { .name = "BLE", .enter = demo_ble_enter, .exit = demo_ble_exit,
      .key = demo_ble_key, .start = demo_ble_start, .stop = demo_ble_stop },
    { .name = "Low Power", .enter = demo_low_power_enter, .exit = demo_low_power_exit,
      .key = demo_low_power_key, .start = demo_low_power_start, .stop = demo_low_power_stop },
};
#define DEMO_COUNT (sizeof(DEMOS) / sizeof(DEMOS[0]))
#define INPUT_QUEUE_DEPTH 8

// 菜单顺序与 s_ok[] 的对应关系用名字固定下来,避免插队时错位。
enum {
    DEMO_COUNTDOWN = 0,
    DEMO_DISPLAY,
    DEMO_BUTTON,
    DEMO_AUDIO,
    DEMO_BATTERY,
    DEMO_WIFI,
    DEMO_BLE,
    DEMO_LOW_POWER,
};

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

// 各外设初始化结果:失败的项在菜单里标 [FAIL] 且不允许进入。
static bool s_ok[DEMO_COUNT];

static lv_obj_t *s_menu_scr;
static lv_obj_t *s_cards[DEMO_COUNT];
static lv_obj_t *s_rows[DEMO_COUNT];
static lv_obj_t *s_mascot;
static demo_navigation_t s_navigation;
static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;
// true 表示当前不在菜单里,而是直接跑宿主应用(开机默认如此)。
static volatile bool s_app_host_mode;

static void menu_refresh(void) {
    for (size_t i = 0; i < DEMO_COUNT; i++) {
        lv_label_set_text_fmt(s_rows[i], "%s%s",
                              DEMOS[i].name,
                              s_ok[i] ? "" : "  [FAIL]");
        ui_pixel_set_selected(s_cards[i], i == s_navigation.selected, s_ok[i]);
        lv_obj_set_style_text_color(s_rows[i],
            s_ok[i] ? lv_color_hex(UI_INK) : lv_color_hex(0x7A2020), 0);
    }
}

static void menu_build(void) {
    s_menu_scr = ui_pixel_screen_create("FoloToy");

    for (size_t i = 0; i < DEMO_COUNT; i++) {
        int x = 11 + (int)(i % 2) * 112;
        int y = 52 + (int)(i / 2) * 47;
        s_cards[i] = ui_pixel_panel_create(s_menu_scr, x, y, 102, 40, UI_PAPER);
        s_rows[i] = lv_label_create(s_cards[i]);
        lv_obj_set_style_text_font(s_rows[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(s_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_rows[i]);
    }

    s_mascot = ui_pixel_mascot_create(s_menu_scr, 101, 242);

    menu_refresh();
    lv_screen_load(s_menu_scr);
}

// 宿主应用请求切到 demo 菜单:先交出按键、停掉它持有的网络/蓝牙/HTTP 服务,
// 再删除页面并重建菜单。全程由 input task 调用,不与菜单逻辑并发。
void app_shell_enter_menu(void) {
    s_app_host_mode = false;

    esp_err_t err = love_app_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "应用服务停止失败: %s", esp_err_to_name(err));
    }

    if (!bsp_lvgl_lock(1000)) return;
    love_app_exit();
    demo_navigation_init(&s_navigation, DEMO_COUNT);
    s_navigation.selected = DEMO_COUNTDOWN;   // 默认停在倒计时那一项,便于返回
    menu_build();
    bsp_lvgl_unlock();
}

// 开机直接进入宿主应用。
static void enter_host_app(void) {
    s_app_host_mode = true;
    if (!bsp_lvgl_lock(1000)) return;
    love_app_enter();
    bsp_lvgl_unlock();

    esp_err_t err = love_app_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "应用服务启动失败: %s", esp_err_to_name(err));
    }
}

static demo_nav_input_t navigation_input(bsp_btn_t btn, bsp_btn_ev_t event) {
    if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) return DEMO_NAV_INPUT_OK_LONG;
    if (event != BSP_BTN_CLICK) return DEMO_NAV_INPUT_OTHER;
    if (btn == BSP_BTN_UP) return DEMO_NAV_INPUT_UP_CLICK;
    if (btn == BSP_BTN_DOWN) return DEMO_NAV_INPUT_DOWN_CLICK;
    if (btn == BSP_BTN_OK) return DEMO_NAV_INPUT_OK_CLICK;
    return DEMO_NAV_INPUT_OTHER;
}

static void process_input(const input_event_t *input) {
    // 宿主应用自己解释全部按键语义(短按/长按都由它决定)。
    if (s_app_host_mode) {
        love_app_key(input->btn, input->event);
        return;
    }

    demo_nav_input_t nav_input = navigation_input(input->btn, input->event);

    if (s_navigation.active >= 0) {
        demo_nav_result_t result = demo_navigation_handle(&s_navigation, nav_input, true);
        const demo_entry_t *demo = &DEMOS[result.index];
        if (result.action == DEMO_NAV_ACTION_EXIT) {
            esp_err_t e = demo->stop ? demo->stop() : ESP_OK;
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "%s 页面停止失败: %s", demo->name, esp_err_to_name(e));
                return;
            }
            if (!bsp_lvgl_lock(500)) return;
            demo->exit();
            demo_navigation_complete_exit(&s_navigation);
            menu_build();
            bsp_lvgl_unlock();
        } else if (result.action == DEMO_NAV_ACTION_FORWARD) {
            demo->key(input->btn, input->event);
        }
        return;
    }

    if (nav_input == DEMO_NAV_INPUT_OTHER || nav_input == DEMO_NAV_INPUT_OK_LONG) return;
    if (!bsp_lvgl_lock(500)) return;
    demo_nav_result_t result = demo_navigation_handle(
        &s_navigation, nav_input, s_ok[s_navigation.selected]);
    if (result.action == DEMO_NAV_ACTION_REFRESH) {
        menu_refresh();
        ui_pixel_mascot_jump(s_mascot);
    } else if (result.action == DEMO_NAV_ACTION_ENTER) {
        const demo_entry_t *demo = &DEMOS[result.index];
        ui_pixel_mascot_jump(s_mascot);
        lv_obj_delete(s_menu_scr);
        s_menu_scr = NULL;
        s_mascot = NULL;
        demo->enter();
        bsp_lvgl_unlock();

        esp_err_t e = demo->start ? demo->start() : ESP_OK;
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "%s 页面启动失败: %s", demo->name, esp_err_to_name(e));
        }
        return;
    }
    bsp_lvgl_unlock();
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            process_input(&input);
        }
    }
}

static esp_err_t input_dispatch_init(void) {
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "demo_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void input_dispatch_deinit(void) {
    s_input_ready = false;
    if (s_input_task) {
        vTaskDelete(s_input_task);
        s_input_task = NULL;
    }
    if (s_input_queue) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
    }
}

// button callbacks run on the shared esp_timer task; enqueue only and return immediately.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy AI Passport BSP demo 启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是本 demo 的 UI 载体,失败就没有菜单可言 —— 打清楚日志后退出,
    // 不做"串口菜单"降级(那会让本文件复杂一倍,违背参考示例的初衷)。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,demo 无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    demo_navigation_init(&s_navigation, DEMO_COUNT);

    // 其余外设单项失败不阻塞:菜单里标 [FAIL],其他项照常可测。
    s_ok[DEMO_COUNTDOWN] = true;                      // 由显示与按键支撑,服务在页面内按需启动
    s_ok[DEMO_DISPLAY] = true;                        // Display 已确认可用
    esp_err_t input_err = input_dispatch_init();
    esp_err_t button_err = input_err == ESP_OK
                         ? bsp_button_init(on_key, NULL)
                         : ESP_ERR_INVALID_STATE;
    s_ok[DEMO_BUTTON] = input_err == ESP_OK && button_err == ESP_OK;
    if (input_err != ESP_OK) {
        ESP_LOGE(TAG, "按键事件任务创建失败: %s", esp_err_to_name(input_err));
    } else if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(button_err));
        input_dispatch_deinit();
    }
    s_ok[DEMO_AUDIO] = (bsp_audio_init() == ESP_OK);
    s_ok[DEMO_BATTERY] = (bsp_battery_init() == ESP_OK);
    s_ok[DEMO_WIFI] = true;                            // 页面内按需初始化并显示错误
    s_ok[DEMO_BLE] = true;
    s_ok[DEMO_LOW_POWER] = true;

    // 开机直接进应用:需要按键可用,否则留在菜单也没有意义。
    if (s_ok[DEMO_BUTTON]) {
        s_input_ready = true;
        enter_host_app();
    } else if (bsp_lvgl_lock(1000)) {
        menu_build();
        bsp_lvgl_unlock();
        s_input_ready = true;
    }

    ESP_LOGI(TAG, "就绪:Display=%d Button=%d Audio=%d Battery=%d",
             s_ok[DEMO_DISPLAY], s_ok[DEMO_BUTTON], s_ok[DEMO_AUDIO], s_ok[DEMO_BATTERY]);
}
