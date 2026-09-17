// main/power_sleep.c —— 浅睡眠 / 深睡眠实现,不含任何界面代码。
//
// 两种模式入睡前均 suspend ES8311;light sleep 返回后显式恢复。
// deep sleep 还会按 CW2017 -> ES8311 -> I2S -> 共享 I2C -> LCD 顺序停止外设,
// 并武装"任意按键唤醒"。这条顺序被 tests/test_deep_sleep_contract.py 逐项断言,
// 改动前先看那个测试。
#include "power_sleep.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power_sleep";

#define LIGHT_SLEEP_TIME_US ((uint64_t)POWER_SLEEP_LIGHT_SECONDS * 1000ULL * 1000ULL)
#define DEEP_SLEEP_MAGIC    0x464F4C4FUL
#define SLEEP_WORKER_STACK 3072

typedef enum {
    SLEEP_COMMAND_LIGHT = 1,
    SLEEP_COMMAND_DEEP,
} sleep_command_t;

// 工作线程随应用存活:本固件没有退出流程,深睡眠成功也是直接重启,
// 所以不需要停止路径,只做懒创建。
static TaskHandle_t s_task;
// 是否有一次休眠正在进行。**所有写入都从 claim_sleep()/release_sleep_claim() 走**
// (见 claim_sleep 的说明),否则"检查并置位"会漏。
static volatile bool s_busy;
// 本次深睡眠的定时唤醒时长;0 = 不设定时器(只靠按键唤醒),见 power_sleep_deep_for()。
// 用静态变量而不是塞进任务通知的值:同时只可能有一次休眠在跑(见 claim_sleep),
// 不必为几秒和半小时编一套打包格式。
static uint32_t s_deep_seconds = POWER_SLEEP_DEEP_SECONDS;
// 最近一次"由深睡唤醒"的原因。单独抄一份的理由见 power_sleep_last_deep_wake_text()。
static RTC_DATA_ATTR uint32_t s_last_deep_wake_cause = ESP_SLEEP_WAKEUP_UNDEFINED;

static portMUX_TYPE s_result_lock = portMUX_INITIALIZER_UNLOCKED;
static power_sleep_result_t s_result;
static RTC_DATA_ATTR uint32_t s_deep_sleep_magic;
static RTC_DATA_ATTR uint32_t s_deep_sleep_count;
// 「谁在睡」那把锁:s_busy 的每一次写入都从 claim_sleep()/release_sleep_claim() 里走,
// 这付锁保护"检查并置位"这一步(见 claim_sleep 的说明)。
static portMUX_TYPE s_request_lock = portMUX_INITIALIZER_UNLOCKED;
static void release_sleep_claim(void);
// 本次深睡是否已经过了**会失败的那一段**(唤醒源武装好、开始关外设)。见 power_sleep_committed()。
static volatile bool s_committed;

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

// 武装"任意按键唤醒深睡"。
//
// 三个按键共用一个 ADC 节点(GPIO0,BSP_BTN_GPIO):按下时 0 / 300 / 595 mV,都低于
// ESP32-C3 的数字低电平门限(约 825 mV);松开时由**板上外部 10k 上拉**抬到 3300 mV。
// 所以配一条**低电平**唤醒源就覆盖三个键,空闲时也不会触发。GPIO0 是 C3 的 RTC GPIO,
// 落在 SOC_GPIO_DEEP_SLEEP_WAKE_VALID_GPIO_MASK 里,具备深睡唤醒能力。
//
// 三件必须同时成立的事:
//
//  1. 内部上下拉一律**不用**。这是个有外部 10k 的分压节点,再叠一层内部 45k 会把三档电平
//     整体挪位(bsp_pins.h 有明文警告),吃掉按键窗口的余量。
//
//  2. 调用前必须先 bsp_button_suspend()。周期采样与 ADC 的输入网络都挂在这个节点上,
//     而唤醒源盯的就是这个节点的电平(见 bsp_button.h 与契约测试的顺序断言)。
//
//  3. sdkconfig 里 CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS 要是 n。IDF 默认会在
//     入睡前**自己**按唤醒电平给这个脚加一层内部上下拉(esp_hw_support/sleep_modes.c 的
//     gpio_deep_sleep_wakeup_prepare),而 esp_sleep.h 点名警告过这种叠加:
//     "when using external pull-up or pull-down resistors, please be sure to disable the
//     ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS option"。
//     (注意别把这条当成"睡下去几秒自己醒"的解释:低电平唤醒那一支加的是**上拉**,方向与板上
//     外部上拉相同,物理上制造不出低电平假唤醒。它只是"这个脚在睡眠期间的状态该由谁决定"
//     的问题 —— 板上有自己的上拉,就不该让 IDF 再插手。)
static esp_err_t arm_button_wake(void)
{
    // 掩码是**位掩码**而不是脚号,这里顺手把"掩码非空"钉在编译期:IDF 5.5.3 对空掩码
    // **不报错**(invalid_io_mask 为 0、校验循环一次都不执行、err 保持 ESP_OK),也就是
    // 静默地一个唤醒源都没装 —— 那种情况下检查返回值也拦不住。
    _Static_assert((1ULL << BSP_BTN_GPIO) != 0, "深睡唤醒掩码不能为空");
    const uint64_t mask = 1ULL << BSP_BTN_GPIO;

    const gpio_config_t config = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,   // 高电平由板上外部 10k 保证(见上)
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;
    // 非 RTC 的脚会返回 ESP_ERR_INVALID_ARG;返回值一路传到调用方,由它决定不睡。
    return esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
}

// 深睡的落地函数(**在工作线程上跑**)。
//
// 顺序上有两处硬契约:
//  1. **先武装唤醒源、并且检查返回值,再关外设**。唤醒源没配上就不睡 —— 官方参考文档
//     记过"没装唤醒源就睡下去 = 再也醒不来",那种失败只能靠断电救。所以这里一旦失败就把
//     按键装回去并返回,让调用方(love_app_sleep_deep)把服务与界面还回去。
//  2. 关外设的顺序 CW2017 → ES8311 → I2S → 共享 I2C → ST7789:前几个共用 I2C,必须等
//     电量计的写-回读做完;LCD 必须在拿到 LVGL 锁之后再关(否则 LVGL 会往已休眠的屏刷)。
static void run_deep_sleep(void)
{
    power_sleep_result_t result = { .attempted = true, .deep = true };
    const char *failure = "Timer wake";

    esp_err_t err = ESP_OK;
    if (s_deep_seconds != 0) {
        err = esp_sleep_enable_timer_wakeup((uint64_t)s_deep_seconds * 1000ULL * 1000ULL);
    }
    if (err == ESP_OK) {
        failure = "Button suspend";
        err = bsp_button_suspend();
    }
    if (err == ESP_OK) {
        failure = "Button wake";
        err = arm_button_wake();
    }
    if (err != ESP_OK) {
        // 没配上唤醒源就不睡:睡下去没人叫得醒它。把按键装回去交还给调用方。
        ESP_LOGE(TAG, "深睡唤醒源武装失败(%s): %s,不睡", failure, esp_err_to_name(err));
        if (bsp_button_resume() != ESP_OK) {
            // 装不回来就只能重启:三个键是这台设备唯一的输入手段,没有"按键按不动"这个状态可留。
            ESP_LOGE(TAG, "按键恢复失败,重启恢复");
            esp_restart();
        }
        result.error = err;
        publish_result(&result);
        return;
    }

    ESP_LOGI(TAG, "深睡:%s,机身任意按键唤醒", s_deep_seconds == 0 ? "只靠按键"
                                                                   : "按键 + RTC 定时器");
    // 过了这里就不会再有"失败返回"这一种结局了:调用方靠这个标志区分"还没走到这一步"和
    // "确实没睡成"(见 power_sleep_committed)。**必须在延时与关外设之前置起**。
    s_committed = true;
    // 让"深睡眠"那句提示先画出去(状态页那条手动操作)。
    vTaskDelay(pdMS_TO_TICKS(250));

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
        if (command == SLEEP_COMMAND_DEEP) {
            run_deep_sleep();
        } else {
            run_light_sleep();
        }
        // 能走到这里就说明**没睡成**(深睡成功不会返回)。把认领放开让下一次能进来。
        release_sleep_claim();
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

// 认领一次休眠。**必须把"看 s_busy"和"置 s_busy"放进同一段临界区**:这条路上有两个
// 不同任务的调用者(输入任务的空闲路径、USB 控制台的 sleep 命令),先查后写的话双方都会
// 通过检查,后来者还会把 s_deep_seconds 覆盖掉 —— 把"只靠按键唤醒"覆盖成"5 秒定时唤醒",
// 设备就变成每 5 秒自重启一次。
// s_busy 的所有写入(认领、放开、工作线程)都走这一对函数,免得谁把新认领覆盖掉。
static bool claim_sleep(void)
{
    bool claimed = false;
    taskENTER_CRITICAL(&s_request_lock);
    if (!s_busy) {
        s_busy = true;
        s_committed = false;
        claimed = true;
    }
    taskEXIT_CRITICAL(&s_request_lock);
    return claimed;
}

static void release_sleep_claim(void)
{
    taskENTER_CRITICAL(&s_request_lock);
    s_busy = false;
    taskEXIT_CRITICAL(&s_request_lock);
}

static esp_err_t request(uint32_t command)
{
    esp_err_t err = ensure_worker();
    if (err != ESP_OK) {
        release_sleep_claim();   // 没排上队,把认领还回去
        return err;
    }
    xTaskNotify(s_task, command, eSetValueWithOverwrite);
    return ESP_OK;
}

esp_err_t power_sleep_light(void)
{
    if (!claim_sleep()) return ESP_ERR_INVALID_STATE;
    return request(SLEEP_COMMAND_LIGHT);
}

esp_err_t power_sleep_deep(void)
{
    return power_sleep_deep_for(POWER_SLEEP_DEEP_SECONDS);
}

esp_err_t power_sleep_deep_for(uint32_t wake_seconds)
{
    if (!claim_sleep()) return ESP_ERR_INVALID_STATE;
    // 认领之后只有本调用者能写它,不会再有竞争;0 = 不设定时器,只用机身按键唤醒(见头文件)。
    s_deep_seconds = wake_seconds;
    return request(SLEEP_COMMAND_DEEP);
}

bool power_sleep_busy(void)
{
    return s_busy;
}

bool power_sleep_committed(void)
{
    return s_committed;
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

static const char *wake_cause_text(uint32_t cause)
{
    switch (cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "上电/复位";
    case ESP_SLEEP_WAKEUP_TIMER:     return "定时唤醒";
    case ESP_SLEEP_WAKEUP_GPIO:      return "按键唤醒";
    default:                         return "其他来源";
    }
}

const char *power_sleep_wake_text(void)
{
    return wake_cause_text((uint32_t)esp_sleep_get_wakeup_cause());
}

const char *power_sleep_last_deep_wake_text(void)
{
    if (s_last_deep_wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED) return NULL;
    return wake_cause_text(s_last_deep_wake_cause);
}

void power_sleep_note_boot(void)
{
    // 抄进 RTC 内存:主机插着 USB 时**开一次串口就可能把芯片复位**
    // (实测 rst:0x15 USB_UART_CHIP_RESET),复位之后实时寄存器就变成"上电/复位"了 ——
    // 不抄一份,之后谁也答不出"刚才是谁把我叫醒的"。所以要在启动最早期叫一次。
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause != ESP_SLEEP_WAKEUP_UNDEFINED) s_last_deep_wake_cause = (uint32_t)cause;
}
