// main/love_shot.c —— 串口截屏实现,协议与取舍见 love_shot.h。
#include "love_shot.h"

#include "bsp_display.h"
#include "bsp_pins.h"

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "love_shot";

// 控制台装驱动时用的是默认配置:tx 环形缓冲 256 字节。**一次提交超过缓冲容量的
// 写入会立刻失败**("data will never ever fit"),所以按 128 字节分块,留一半余量。
#define SHOT_CHUNK 128
#define SHOT_WRITE_TIMEOUT_MS 500
// 整幅图的总等待上限。153,600 字节走 USB 全速,正常在 1 秒上下。
#define SHOT_TOTAL_TIMEOUT_MS 15000

static volatile bool s_capturing;
static bool s_hook_added;
static int32_t s_next_y;   // 下一块应当从哪一行开始(用来验证分块是连续整宽的)
static size_t s_sent;
static SemaphoreHandle_t s_done;
static volatile bool s_ok;

// 分块写。任何一块失败(主机拔线、超时)就放弃本次传输,但调用方照常活着。
static bool write_raw(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t left = len;
    while (left > 0) {
        const size_t n = left < SHOT_CHUNK ? left : SHOT_CHUNK;
        const int written = usb_serial_jtag_write_bytes(p, n,
                                                       pdMS_TO_TICKS(SHOT_WRITE_TIMEOUT_MS));
        if (written <= 0) return false;
        p += written;
        left -= (size_t)written;
    }
    return true;
}

// 挂在 LV_EVENT_FLUSH_START 上(BSP 已经用它做圆角遮罩)。不截图时立刻返回。
static void capture_flush(lv_event_t *event)
{
    if (!s_capturing) return;

    lv_display_t *disp = lv_event_get_target(event);
    const lv_area_t *area = (const lv_area_t *)lv_event_get_param(event);
    const lv_draw_buf_t *draw_buf = lv_display_get_buf_active(disp);
    if (!area || !draw_buf || !draw_buf->data) {
        s_capturing = false;
        return;
    }

    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    // 只有"整宽 + 自顶向下首尾相接"的一串分块拼起来才是整幅图。本板绘制缓冲是
    // 240×20,整屏重绘正好 16 条这样的带;一旦不是这个形状,立即中止 ——
    // 发一张错位的图比不发更糟。
    if (w != BSP_LCD_W || area->x1 != 0 || area->y1 != s_next_y) {
        s_capturing = false;
        return;
    }
    if (draw_buf->header.stride < (uint32_t)w * sizeof(uint16_t)) {
        s_capturing = false;
        return;
    }

    // 逐行发:不去假设 stride 恰好等于 w*2(绘制缓冲是这么分配的,但别把假设写死)。
    for (int32_t y = 0; y < h; y++) {
        const uint8_t *row = draw_buf->data + (size_t)y * draw_buf->header.stride;
        if (!write_raw(row, (size_t)w * sizeof(uint16_t))) {
            s_capturing = false;
            return;
        }
    }

    s_sent += (size_t)w * (size_t)h * sizeof(uint16_t);
    s_next_y = area->y2 + 1;
}

// 真正的抓图。**必须跑在 LVGL 任务里**:整屏软件渲染要 ~7KB 栈,而串口控制台
// 任务只有 4KB —— 实测在那里直接触发栈保护复位(SP 掉到栈边界之外)。
// 由 love_shot_send() 用 lv_async_call() 投递,调用时 LVGL 锁已held(LVGL 任务上下文)。
static void shot_async(void *unused)
{
    (void)unused;

    lv_display_t *disp = lv_display_get_default();
    const size_t total = (size_t)BSP_LCD_W * BSP_LCD_H * sizeof(uint16_t);

    char header[64];
    const int header_len = snprintf(header, sizeof(header),
                                    "FAP_SCREENSHOT_V1 %d %d RGB565LE %u\n",
                                    BSP_LCD_W, BSP_LCD_H, (unsigned)total);

    bool ok = false;
    if (disp && header_len > 0 && header_len < (int)sizeof(header)) {
        s_sent = 0;
        s_next_y = 0;
        s_capturing = true;

        // 头行先发:整屏重绘要好几秒,发完再渲染的话主机得多等一轮;失败时主机读不满
        // 声明字节数,会以"数据提前结束"报错 —— 是可检测的干净失败,不是错位的图。
        if (write_raw(header, (size_t)header_len)) {
            lv_obj_invalidate(lv_screen_active());
            lv_refr_now(disp);
            // s_capturing 仍为真 = 每一块都合规且发完了。
            ok = s_capturing && s_sent == total;
            s_capturing = false;
        }
    }

    s_ok = ok;
    // 这里**不打日志**:此刻日志级别还是静默的,打了也看不见,而且会挤进二进制流。
    // 失败原因交给调用方在恢复日志级别之后报。
    if (s_done) xSemaphoreGive(s_done);
}

bool love_shot_send(void)
{
    // 驱动由控制台 REPL 装(见 love_console_start)。没装就去读会解引用空对象,
    // 本板上表现为反复重启、屏幕闪 —— 所以先确认再动。
    if (!usb_serial_jtag_is_driver_installed()) {
        ESP_LOGW(TAG, "USB-Serial-JTAG 驱动未安装,不截图");
        return false;
    }
    if (!usb_serial_jtag_is_connected()) {
        ESP_LOGW(TAG, "USB 主机未连接,不截图");
        return false;
    }
    if (!lv_display_get_default()) {
        ESP_LOGW(TAG, "没有可截图的显示");
        return false;
    }

    if (!s_done) s_done = xSemaphoreCreateBinary();
    if (!s_done) return false;

    // 二进制窗口期间必须一个日志字节都不能混进来:主机是按声明字节数精确读取的。
    const esp_log_level_t saved_level = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);

    s_ok = false;
    bool queued = false;
    if (bsp_lvgl_lock(500)) {
        if (!s_hook_added) {
            // 只挂一次,之后靠 s_capturing 开关。
            lv_display_add_event_cb(lv_display_get_default(), capture_flush,
                                    LV_EVENT_FLUSH_START, NULL);
            s_hook_added = true;
        }
        // 渲染交给 LVGL 任务去做(栈够),这里只等它做完。
        lv_async_call(shot_async, NULL);
        queued = true;
        bsp_lvgl_unlock();
    }

    bool done = false;
    if (queued) {
        done = xSemaphoreTake(s_done, pdMS_TO_TICKS(SHOT_TOTAL_TIMEOUT_MS)) == pdTRUE;
    } else {
        s_capturing = false;   // 没投递出去,别把标志留在置位状态
    }

    esp_log_level_set("*", saved_level);

    if (!queued) {
        ESP_LOGW(TAG, "拿不到 LVGL 锁,未截图");
    } else if (!done) {
        s_capturing = false;
        ESP_LOGW(TAG, "截屏超时:LVGL 任务没有完成渲染");
    } else if (!s_ok) {
        ESP_LOGW(TAG, "截屏未完成(已发 %u 字节)", (unsigned)s_sent);
    }
    return queued && done && s_ok;
}
