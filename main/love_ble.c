// main/love_ble.c —— 蓝牙串口控制台(NUS)实现,见 love_ble.h 的说明。
//
// 这里有三条必须守住的规矩,破了任何一条都会变成死锁或卡死:
//   1. GATT 写回调与 GAP 事件回调都跑在 NimBLE host 任务里 —— **只准入队**,
//      不许打印、不许取 LVGL 锁、不许直接执行命令:这个任务就是整个协议栈,
//      它一慢连接就要超时。
//   2. nimble_port_stop() 是**无超时**等待 host 任务退出的,只能在普通任务里调,
//      绝不能在 host 任务里调(等自己 = 死锁)。
//   3. 控制台任务的 4KB 栈必须在 nimble_port_init() **之前**申请 —— NimBLE 一起来
//      堆就碎了,后面再要一块连续 4KB 会失败(love_app 里的 USB 控制台踩过这个坑)。
#include "love_ble.h"

#include "love_console.h"
#include "love_console_line.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "love_ble";

#define BLE_STOP_TIMEOUT_MS  3000
// 没人连着满 5 分钟就自动关掉,把 NimBLE 占的那 ~51KB 堆还回去。
#define BLE_IDLE_TIMEOUT_S   300
// 关栈前先把上一条回复发出去(notify 是异步排队的)。
#define BLE_CLOSE_GRACE_MS   300

#define BLE_CMD_QUEUE_LEN    4
#define BLE_CONSOLE_STACK    4096
#define BLE_CONSOLE_PRIO     3
#define BLE_CONSOLE_TICK_MS  1000

// 一次写入的扁平化缓冲。MTU 上限 256,ATT 载荷最多 253 字节,256 够用。
#define BLE_WRITE_MAX        256

// 标准 NUS(Nordic UART Service)。字节按小端逆序写,与 ESP-IDF 组件内自带的
// bleuart 服务源码逐字节一致;用标准 UUID 是为了让手机上的现成串口 App
// (Serial Bluetooth Terminal 等)不必手配 UUID 就能收发。
static const ble_uuid128_t NUS_SVC_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
// 6E400002 手机 → 设备(写入命令)
static const ble_uuid128_t NUS_RX_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
// 6E400003 设备 → 手机(通知回显)
static const ble_uuid128_t NUS_TX_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static char s_device_name[24];
static SemaphoreHandle_t s_host_stopped;
static uint8_t s_addr_type;
static bool s_initialized;
static bool s_advertising;
static bool s_stop_in_progress;
static bool s_host_done;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;
static love_line_t s_line;
static QueueHandle_t s_cmd_queue;
static TaskHandle_t s_console_task;
static volatile uint32_t s_idle_since_s;
static volatile bool s_stop_requested;
// 正在拆栈。ble_gap_adv_stop() 与 ble_gap_terminate() 都会回调到这里,
// 那两个回调里的"重开广播"必须被挡住,否则关蓝牙的途中又把广播打开了。
static volatile bool s_shutting_down;
static void (*s_shutdown_cb)(void);

// 只保护 start/stop 的"谁在改状态",不做成互斥量:NimBLE 起来之前也可能被调用,
// 而那时还没有堆给 xSemaphoreCreateMutex()。临界区里只有几个赋值,没有阻塞调用。
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_transitioning;

static int advertise(void);
static int gap_event(struct ble_gap_event *event, void *arg);

/* ---------- 状态 ---------- */

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void note_idle_start(void)
{
    s_idle_since_s = now_s();
}

bool love_ble_running(void)
{
    return s_initialized;
}

bool love_ble_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

const char *love_ble_state_text(void)
{
    if (!s_initialized) return "未开启";
    if (love_ble_connected()) return "已连接";
    return s_advertising ? "广播中" : "启动中";
}

uint32_t love_ble_idle_seconds(void)
{
    if (love_ble_connected()) return 0;
    // 32 位无符号回绕后相减仍然得到正确的间隔。
    return now_s() - s_idle_since_s;
}

void love_ble_set_shutdown_cb(void (*fn)(void))
{
    s_shutdown_cb = fn;
}

void love_ble_request_stop(void)
{
    s_stop_requested = true;
}

size_t love_ble_host_stack_headroom(void)
{
    // 任务名由 IDF 的 NimBLE porting 层写死为 "nimble_host"。
    TaskHandle_t host = xTaskGetHandle("nimble_host");
    if (!host) return 0;
    return (size_t)uxTaskGetStackHighWaterMark(host) * sizeof(StackType_t);
}

/* ---------- 设备 → 手机 ---------- */

// 由 love_console 的输出出口调用。可能来自 BLE 控制台任务,也可能来自 USB REPL 任务
// (USB 上敲的命令在 USB 上看得见,同时镜像给手机一份)。
static void ble_notify(const char *text, size_t len)
{
    if (!text || len == 0) return;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;

    // 未协商完成时是 ATT 默认 MTU 23,一次只能装 20 字节;协商后按实际 MTU 走。
    // 超过 MTU 的报文会被栈直接拒掉(不会自动分片),所以要自己切。
    const uint16_t mtu = ble_att_mtu(s_conn_handle);
    const size_t chunk = (mtu > 3) ? (size_t)(mtu - 3) : 20;

    for (size_t off = 0; off < len; off += chunk) {
        const size_t n = (len - off < chunk) ? (len - off) : chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(text + off, n);
        if (!om) {
            ESP_LOGW(TAG, "通知发送失败:没有可用的 mbuf");
            return;
        }
        const int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "通知发送失败: %d", rc);
            return;
        }
        // 分片之间让一拍:手机读得慢时 mbuf 池会被打空,后面几片就直接丢了。
        if (off + n < len) vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ---------- 手机 → 设备 ---------- */

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    // 只有 RX 特征可写,这里再核对一次 UUID,避免误写 TX。
    if (!ctxt->chr || !ctxt->chr->uuid ||
        ble_uuid_cmp(ctxt->chr->uuid, &NUS_RX_UUID.u) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t payload[BLE_WRITE_MAX] = { 0 };
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, payload, sizeof(payload), &len) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    // 本回调在 host 任务里,**只允许**做这几件事:攒行、入队。见文件头的规矩 1。
    // 队列可能不存在(控制台任务没建起来,BLE 仍会以降级方式启动),那就只能丢掉。
    for (uint16_t i = 0; i < len; i++) {
        if (love_line_feed(&s_line, (char)payload[i])) {
            if (!s_cmd_queue) continue;
            char line[LOVE_LINE_MAX];
            memcpy(line, s_line.buf, sizeof(line));
            if (xQueueSend(s_cmd_queue, line, 0) != pdTRUE) {
                ESP_LOGW(TAG, "命令队列满,丢弃一行");
            }
        }
    }
    return 0;
}

/* ---------- GATT 服务 ---------- */

static const struct ble_gatt_svc_def GATT_SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                // TX:设备 → 手机。NUS 的惯例是它排在前面,而且必须留 val_handle
                // 才能在收到订阅后主动发通知(通知 API 要的就是这个句柄)。
                .uuid = &NUS_TX_UUID.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_handle,
            },
            {
                // RX:手机 → 设备。notify 之外还要允许无响应写,手机 App 默认这么发。
                .uuid = &NUS_RX_UUID.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ---------- GAP ---------- */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_advertising = false;
        } else {
            // 连接没能建立,广播已被控制器停掉,得自己重开。
            s_advertising = false;
            if (!s_shutting_down) (void)advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        note_idle_start();
        // 不重开广播,设备从此就再也搜不到了(而 s_advertising 还会骗人地停在 true)。
        if (!s_shutting_down) (void)advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_advertising = false;
        if (!s_shutting_down) (void)advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        // 手机订阅了 TX 才算真正能收通知,这时才把用法推过去:
        // 订阅之前发的通知会被对端直接丢掉。
        if (event->subscribe.attr_handle == s_tx_handle && s_cmd_queue) {
            char usage[LOVE_LINE_MAX] = "help";
            (void)xQueueSend(s_cmd_queue, usage, 0);
        }
        return 0;

    default:
        return 0;
    }
}

static int advertise(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)s_device_name;
    fields.name_len = (uint8_t)strlen(s_device_name);
    fields.name_is_complete = 1;
    // 刻意**不**把 128 位服务 UUID 放进广播包:31 字节的包里 flags(3)+
    // 名字(17)+ 128 位 UUID(18)已经超了,ble_gap_adv_set_fields() 会返回 EMSGSIZE。
    // 手机 App 靠名字就能连,不需要从广播里发现 UUID。

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return rc;

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;   // 可连接,才能收发命令
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc == 0) s_advertising = true;
    return rc;
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE 复位: %d", reason);
    s_advertising = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc == 0) rc = advertise();
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE 广播失败: %d", rc);
        s_advertising = false;
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    if (s_host_stopped) xSemaphoreGive(s_host_stopped);
    nimble_port_freertos_deinit();
}

/* ---------- 控制台任务 ---------- */

static void request_shutdown(void)
{
    // 关蓝牙要写回配置、刷新界面,那些都是 love_app 的事。用回调而不是直接调用,
    // 保持依赖单向:只有 love_app 认识 love_ble。
    if (s_shutdown_cb) s_shutdown_cb();
}

static void console_task(void *arg)
{
    (void)arg;
    char line[LOVE_LINE_MAX];

    for (;;) {
        if (xQueueReceive(s_cmd_queue, line, pdMS_TO_TICKS(BLE_CONSOLE_TICK_MS)) == pdTRUE) {
            (void)love_console_exec(line, LOVE_CONSOLE_SRC_BLE);
            if (s_stop_requested) {
                // 从 BLE 链路敲的 ble off:回复刚排进发送队列,等它发出去再关栈。
                s_stop_requested = false;
                vTaskDelay(pdMS_TO_TICKS(BLE_CLOSE_GRACE_MS));
                request_shutdown();
            }
            continue;
        }

        // 超时分支 = 空闲判定。只在**无人连接**时计时:手机连着就一直开着。
        if (s_initialized && !love_ble_connected() &&
            love_ble_idle_seconds() >= BLE_IDLE_TIMEOUT_S) {
            ESP_LOGI(TAG, "无人连接已 %u 秒,自动关闭蓝牙", (unsigned)BLE_IDLE_TIMEOUT_S);
            note_idle_start();   // 关掉之后回调可能没生效,别让它每个 tick 都触发
            request_shutdown();
        }
    }
}

// 任务**永不删除**:删掉它要处理"任务删自己"和重复创建两类麻烦,而它只在
// 第一次开蓝牙时才建,之后常驻 4KB 换掉的是整个 NimBLE 的 51KB。
static void ensure_console_task(void)
{
    if (s_console_task) return;

    if (!s_cmd_queue) {
        s_cmd_queue = xQueueCreate(BLE_CMD_QUEUE_LEN, LOVE_LINE_MAX);
        if (!s_cmd_queue) {
            ESP_LOGE(TAG, "创建蓝牙命令队列失败");
            return;
        }
    }

    if (xTaskCreate(console_task, "love_ble_con", BLE_CONSOLE_STACK, NULL,
                    BLE_CONSOLE_PRIO, &s_console_task) != pdPASS) {
        s_console_task = NULL;
        ESP_LOGE(TAG, "创建蓝牙控制台任务失败(堆余 %u,最大连续块 %u):"
                      "蓝牙仍可用,但没有串口控制台也不会空闲自动关",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
}

/* ---------- 启停 ---------- */

void love_ble_device_name(char *buf, size_t size)
{
    if (!buf || size == 0) return;
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(buf, size, "LoveCount-%02X%02X", mac[4], mac[5]);
}

static bool begin_transition(void)
{
    bool ok = false;
    portENTER_CRITICAL(&s_state_mux);
    if (!s_transitioning) {
        s_transitioning = true;
        ok = true;
    }
    portEXIT_CRITICAL(&s_state_mux);
    return ok;
}

static void end_transition(void)
{
    portENTER_CRITICAL(&s_state_mux);
    s_transitioning = false;
    portEXIT_CRITICAL(&s_state_mux);
}

esp_err_t love_ble_start(void)
{
    if (s_initialized) return ESP_OK;
    if (!begin_transition()) {
        // 另一个任务正在开或关。调用方(按键、网页、控制台)各有提示,这里不必再报。
        ESP_LOGW(TAG, "蓝牙正在切换状态,忽略这次启动");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;
    love_ble_device_name(s_device_name, sizeof(s_device_name));
    love_line_reset(&s_line);
    s_stop_requested = false;
    s_shutting_down = false;
    note_idle_start();

    // 先建控制台任务(要连续 4KB),再起 NimBLE —— 顺序反了就再也拿不到这块栈。
    ensure_console_task();

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        end_transition();
        return err;
    }
    s_initialized = true;

    s_host_stopped = xSemaphoreCreateBinary();
    if (!s_host_stopped) {
        nimble_port_deinit();
        s_initialized = false;
        end_transition();
        return ESP_ERR_NO_MEM;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(GATT_SERVICES);
    if (rc == 0) rc = ble_gatts_add_svcs(GATT_SERVICES);
    if (rc == 0) rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "注册 GATT 服务失败: %d", rc);
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
        nimble_port_deinit();
        s_initialized = false;
        end_transition();
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);

    // 命令的输出出口:控制台的每一行同时镜像到手机。
    love_console_set_out(ble_notify);

    ESP_LOGI(TAG, "BLE 已启动,广播名 %s", s_device_name);
    end_transition();
    return ESP_OK;
}

esp_err_t love_ble_stop(void)
{
    if (!s_initialized) return ESP_OK;
    if (!begin_transition()) {
        ESP_LOGW(TAG, "蓝牙正在切换状态,忽略这次关闭");
        return ESP_ERR_INVALID_STATE;
    }

    // 先摘掉输出出口,别再有新的通知往这条要断的连接上发。
    love_console_set_out(NULL);

    if (!s_stop_in_progress) {
        // 先立起"正在拆"的旗子:下面的 adv_stop / terminate 会触发 GAP 回调,
        // 那些回调默认会重开广播,旗子没立就会被它们把广播又打开。
        s_shutting_down = true;
        if (s_advertising) {
            (void)ble_gap_adv_stop();
            s_advertising = false;
        }
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            // 主动断开:否则 nimble_port_stop() 要等连接超时,那可能要好几秒。
            (void)ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        int rc = nimble_port_stop();
        if (rc != 0) {
            ESP_LOGE(TAG, "nimble_port_stop 失败: %d", rc);
            s_shutting_down = false;
            end_transition();
            return ESP_FAIL;
        }
        s_stop_in_progress = true;
    }

    if (!s_host_done) {
        if (!s_host_stopped ||
            xSemaphoreTake(s_host_stopped, pdMS_TO_TICKS(BLE_STOP_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "等待 NimBLE host 停止超时");
            s_shutting_down = false;
            end_transition();
            return ESP_ERR_TIMEOUT;
        }
        s_host_done = true;
    }

    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_deinit 失败: %s", esp_err_to_name(err));
        s_shutting_down = false;
        end_transition();
        return err;
    }

    if (s_host_stopped) {
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
    }
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_initialized = false;
    s_stop_in_progress = false;
    s_host_done = false;
    s_shutting_down = false;
    end_transition();
    return ESP_OK;
}
