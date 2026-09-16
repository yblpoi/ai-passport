// main/love_ble.c —— NimBLE 外设 + 对时 GATT 服务实现。
#include "love_ble.h"

#include "love_time.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "love_ble";

#define BLE_STOP_TIMEOUT_MS 3000
// 时间戳报文最长 20 字节(十进制 10 位 + 余量)。
#define TIME_PAYLOAD_MAX 20

static char s_device_name[24];
static SemaphoreHandle_t s_host_stopped;
static uint8_t s_addr_type;
static bool s_initialized;
static bool s_advertising;
static bool s_stop_in_progress;
static bool s_host_done;

void love_ble_device_name(char *buf, size_t size)
{
    if (!buf || size == 0) return;
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(buf, size, "LoveCount-%02X%02X", mac[4], mac[5]);
}

// 接受三种写法,方便不同工具:8 字节小端、4 字节小端、十进制 ASCII。
static bool parse_epoch(const uint8_t *data, uint16_t len, uint64_t *out)
{
    if (len == 0 || !out) return false;

    if (len == 8) {
        uint64_t value = 0;
        for (int i = 7; i >= 0; i--) value = (value << 8) | data[i];
        *out = value;
        return true;
    }
    if (len == 4) {
        uint32_t value = 0;
        for (int i = 3; i >= 0; i--) value = (value << 8) | data[i];
        *out = value;
        return true;
    }
    if (len < TIME_PAYLOAD_MAX) {
        char text[TIME_PAYLOAD_MAX + 1] = { 0 };
        memcpy(text, data, len);
        text[len] = '\0';
        char *end = NULL;
        unsigned long long value = strtoull(text, &end, 10);
        if (end == text || value == 0) return false;
        *out = (uint64_t)value;
        return true;
    }
    return false;
}

static void fill_state(char *buf, size_t size)
{
    love_time_state_t state;
    love_time_get(&state);
    if (!state.holds) {
        snprintf(buf, size, "time=0 synced=0 请写入 Unix 时间戳(秒)");
        return;
    }
    char described[48] = { 0 };
    love_time_describe(&state, described, sizeof(described));
    snprintf(buf, size, "time=%llu synced=1 %s",
             (unsigned long long)state.epoch_seconds, described);
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // 只有时间特征声明了写权限,这里再核对一次 UUID,避免误写状态特征。
        if (ctxt->chr && ctxt->chr->uuid &&
            ble_uuid_u16(ctxt->chr->uuid) == LOVE_BLE_TIME_UUID) {
            uint8_t payload[TIME_PAYLOAD_MAX + 1] = { 0 };
            uint16_t len = 0;
            if (ble_hs_mbuf_to_flat(ctxt->om, payload, TIME_PAYLOAD_MAX, &len) != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            uint64_t epoch = 0;
            if (!parse_epoch(payload, len, &epoch)) {
                ESP_LOGW(TAG, "时间报文无法解析");
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            if (love_time_set(epoch, LOVE_TIME_SRC_BLE) != ESP_OK) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            ESP_LOGI(TAG, "收到 BLE 对时");
            return 0;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char text[96] = { 0 };
        fill_state(text, sizeof(text));
        return os_mbuf_append(ctxt->om, text, strlen(text)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def GATT_SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(LOVE_BLE_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                // 写入时间:8 字节小端 / 4 字节小端 / 十进制 ASCII 均可。
                .uuid = BLE_UUID16_DECLARE(LOVE_BLE_TIME_UUID),
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // 读状态:回读当前时间与来源。
                .uuid = BLE_UUID16_DECLARE(LOVE_BLE_STATE_UUID),
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 },
        },
    },
    { 0 },
};

static int advertise(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)s_device_name;
    fields.name_len = (uint8_t)strlen(s_device_name);
    fields.name_is_complete = 1;

    const ble_uuid16_t svc_uuid = BLE_UUID16_INIT(LOVE_BLE_SVC_UUID);
    fields.uuids16 = &svc_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return rc;

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;   // 可连接,才能写时间特征
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, NULL, NULL);
    if (rc == 0) s_advertising = true;
    return rc;
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE 复位: %d", reason);
    s_advertising = false;
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

bool love_ble_ready(void)
{
    return s_initialized && s_advertising;
}

esp_err_t love_ble_start(void)
{
    if (s_initialized) return ESP_OK;

    love_ble_device_name(s_device_name, sizeof(s_device_name));

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    s_initialized = true;

    s_host_stopped = xSemaphoreCreateBinary();
    if (!s_host_stopped) {
        nimble_port_deinit();
        s_initialized = false;
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
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE 已启动,广播名 %s", s_device_name);
    return ESP_OK;
}

esp_err_t love_ble_stop(void)
{
    if (!s_initialized) return ESP_OK;

    if (!s_stop_in_progress) {
        if (s_advertising) {
            (void)ble_gap_adv_stop();
            s_advertising = false;
        }
        int rc = nimble_port_stop();
        if (rc != 0) {
            ESP_LOGE(TAG, "nimble_port_stop 失败: %d", rc);
            return ESP_FAIL;
        }
        s_stop_in_progress = true;
    }

    if (!s_host_done) {
        if (!s_host_stopped ||
            xSemaphoreTake(s_host_stopped, pdMS_TO_TICKS(BLE_STOP_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "等待 NimBLE host 停止超时");
            return ESP_ERR_TIMEOUT;
        }
        s_host_done = true;
    }

    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_deinit 失败: %s", esp_err_to_name(err));
        return err;
    }

    if (s_host_stopped) {
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
    }
    s_initialized = false;
    s_stop_in_progress = false;
    s_host_done = false;
    return ESP_OK;
}
