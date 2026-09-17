// main/love_net.c —— Wi-Fi 管理实现。
#include "love_net.h"

#include "love_store.h"
#include "love_time.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "love_net";

#define AP_SSID_PREFIX "LoveCount"
#define AP_DEFAULT_TIMEOUT_S 300

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static SemaphoreHandle_t s_lock;

static bool s_inited;
static bool s_wifi_inited;
static bool s_wifi_started;
static bool s_ap_requested;
static love_net_state_t s_state = LOVE_NET_OFF;
static char s_ip[16];
static int s_rssi = -127;
static char s_sta_ssid[33];

static love_net_ap_t s_scan[LOVE_NET_SCAN_MAX];
static size_t s_scan_count;
static volatile bool s_scan_pending;

static TickType_t s_ap_deadline;

static void lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

// 热点 SSID/密码由 MAC 派生,避免所有设备都是同一个名字;密码显示在设备屏幕上。
static void build_ap_identity(char *ssid, size_t ssid_size, char *pass, size_t pass_size)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ssid, ssid_size, "%s-%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
    snprintf(pass, pass_size, "love%02x%02x", mac[4], mac[5]);
}

static void apply_mode(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (s_ap_requested && s_sta_ssid[0] != '\0') {
        mode = WIFI_MODE_APSTA;
    } else if (s_ap_requested) {
        mode = WIFI_MODE_AP;
    } else if (s_sta_ssid[0] != '\0') {
        mode = WIFI_MODE_STA;
    }

    if (mode == WIFI_MODE_NULL) {
        // 没有热点需求也没有凭据:停掉射频省电,后台仍可通过设置页重新开热点。
        if (s_wifi_started) {
            esp_wifi_stop();
            s_wifi_started = false;
        }
        s_state = LOVE_NET_IDLE;
        return;
    }

    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
        return;
    }

    wifi_mode_t current = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&current) != ESP_OK) return;
    if (current != mode) {
        // 模式切换后由 WIFI_EVENT_STA_START 触发连接,这里不重复配置 STA。
        ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    }
}

static void start_ap_profile(void)
{
    wifi_config_t config = { 0 };
    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    build_ap_identity(ssid, sizeof(ssid), pass, sizeof(pass));

    memcpy(config.ap.ssid, ssid, strlen(ssid));
    config.ap.ssid_len = (uint8_t)strlen(ssid);
    memcpy(config.ap.password, pass, strlen(pass));
    config.ap.max_connection = 2;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.channel = 1;
    (void)esp_wifi_set_config(WIFI_IF_AP, &config);

    ESP_LOGI(TAG, "热点已就绪: %s (密码见设备屏幕)", ssid);
}

static void connect_sta(void)
{
    if (s_sta_ssid[0] == '\0') return;

    char pass[LOVE_WIFI_PASS_MAX] = { 0 };
    if (love_store_load_wifi(s_sta_ssid, sizeof(s_sta_ssid), pass, sizeof(pass)) != ESP_OK) {
        s_sta_ssid[0] = '\0';
        return;
    }

    wifi_config_t config = { 0 };
    memcpy(config.sta.ssid, s_sta_ssid, strlen(s_sta_ssid));
    memcpy(config.sta.password, pass, strlen(pass));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    (void)esp_wifi_set_config(WIFI_IF_STA, &config);

    s_state = LOVE_NET_CONNECTING;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "发起连接失败: %s", esp_err_to_name(err));
        s_state = LOVE_NET_FAILED;
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        connect_sta();
        break;
    case WIFI_EVENT_STA_CONNECTED:
        s_state = LOVE_NET_CONNECTING;
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "STA 断开(原因 %d)", event ? event->reason : -1);
        lock();
        s_ip[0] = '\0';
        s_rssi = -127;
        s_state = LOVE_NET_FAILED;
        unlock();
        // 断开就重连,由 Wi-Fi 驱动做退避;热点不受影响。
        if (s_sta_ssid[0] != '\0') (void)esp_wifi_connect();
        break;
    }
    case WIFI_EVENT_SCAN_DONE: {
        // 静态缓冲:扫描同一时刻只会有一份,避免占用事件任务的栈。
        static wifi_ap_record_t records[LOVE_NET_SCAN_MAX];
        memset(records, 0, sizeof(records));
        uint16_t count = LOVE_NET_SCAN_MAX;
        uint16_t total = 0;
        lock();
        if (esp_wifi_scan_get_ap_num(&total) == ESP_OK &&
            esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
            if (count > LOVE_NET_SCAN_MAX) count = LOVE_NET_SCAN_MAX;
            for (uint16_t i = 0; i < count; i++) {
                snprintf(s_scan[i].ssid, sizeof(s_scan[i].ssid), "%s",
                         (const char *)records[i].ssid);
                s_scan[i].rssi = records[i].rssi;
                s_scan[i].secure = records[i].authmode != WIFI_AUTH_OPEN;
            }
            s_scan_count = count;
        } else {
            s_scan_count = 0;
        }
        s_scan_pending = false;
        unlock();
        break;
    }
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;

    const ip_event_got_ip_t *event = data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));

    wifi_ap_record_t ap = { 0 };
    int rssi = -127;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    lock();
    snprintf(s_ip, sizeof(s_ip), "%s", ip);
    s_rssi = rssi;
    s_state = LOVE_NET_CONNECTED;
    unlock();

    ESP_LOGI(TAG, "已联网并取得 IP: %s", ip);
    love_time_sntp_start();
}

esp_err_t love_net_init(void)
{
    if (s_inited) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    // 事件循环与 netif 可能已由其他模块建好,重复初始化要容忍 ESP_ERR_INVALID_STATE。
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "创建默认事件循环失败: %s", esp_err_to_name(err));
        return err;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif || !s_ap_netif) {
        ESP_LOGE(TAG, "创建 netif 失败");
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    s_wifi_inited = true;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, &s_wifi_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, &s_ip_handler));

    // 凭据由本模块自己管理,不用 Wi-Fi 驱动的 NVS 存储。
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    start_ap_profile();

    char pass[LOVE_WIFI_PASS_MAX] = { 0 };
    bool has_creds = love_store_load_wifi(s_sta_ssid, sizeof(s_sta_ssid),
                                          pass, sizeof(pass)) == ESP_OK &&
                     s_sta_ssid[0] != '\0';
    if (!has_creds) {
        s_sta_ssid[0] = '\0';
        // 没配过网:开机就开热点,否则用户没有任何入口进后台。
        s_ap_requested = true;
    }
    love_net_ap_touch();

    apply_mode();
    s_inited = true;
    s_state = s_sta_ssid[0] ? LOVE_NET_CONNECTING : LOVE_NET_IDLE;
    ESP_LOGI(TAG, "Wi-Fi 已启动(已配网=%d)", (int)has_creds);
    return ESP_OK;
}

void love_net_deinit(void)
{
    if (!s_inited) return;

    love_time_sntp_stop();
    if (s_wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_scan_stop();
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_inited) {
        esp_wifi_deinit();
        s_wifi_inited = false;
    }
    if (s_wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler = NULL;
    }
    if (s_ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_ip_handler = NULL;
    }
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    if (s_lock) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    s_ap_requested = false;
    s_inited = false;
    s_state = LOVE_NET_OFF;
    s_ip[0] = '\0';
}

esp_err_t love_net_ap_start(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    s_ap_requested = true;
    love_net_ap_touch();
    apply_mode();
    return ESP_OK;
}

esp_err_t love_net_ap_stop(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    s_ap_requested = false;
    apply_mode();
    return ESP_OK;
}

esp_err_t love_net_set_credentials(const char *ssid, const char *pass)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!pass) pass = "";

    esp_err_t err = love_store_save_wifi(ssid, pass);
    if (err != ESP_OK) return err;

    snprintf(s_sta_ssid, sizeof(s_sta_ssid), "%s", ssid);
    apply_mode();
    if (s_wifi_started) {
        esp_wifi_disconnect();
        connect_sta();
    }
    return ESP_OK;
}

esp_err_t love_net_forget(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    (void)love_store_clear_wifi();
    s_sta_ssid[0] = '\0';
    s_ip[0] = '\0';
    s_state = LOVE_NET_IDLE;
    if (s_wifi_started) esp_wifi_disconnect();

    // 清除凭据后必须留下配网入口。
    return love_net_ap_start();
}

void love_net_get_status(love_net_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    lock();
    out->state = s_state;
    out->ap_active = s_ap_requested;
    snprintf(out->ip, sizeof(out->ip), "%s", s_ip);
    out->rssi = s_rssi;
    snprintf(out->sta_ssid, sizeof(out->sta_ssid), "%s", s_sta_ssid);
    unlock();

    out->has_credentials = out->sta_ssid[0] != '\0';
    build_ap_identity(out->ap_ssid, sizeof(out->ap_ssid),
                      out->ap_pass, sizeof(out->ap_pass));
    snprintf(out->site_url, sizeof(out->site_url), "http://192.168.4.1");
}

esp_err_t love_net_scan_start(void)
{
    if (!s_inited || !s_wifi_started) return ESP_ERR_INVALID_STATE;
    if (s_scan_pending) return ESP_ERR_INVALID_STATE;

    s_scan_pending = true;
    s_scan_count = 0;
    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err != ESP_OK) s_scan_pending = false;
    return err;
}

bool love_net_scan_pending(void)
{
    return s_scan_pending;
}

esp_err_t love_net_scan_results(love_net_ap_t *out, size_t max, size_t *count)
{
    if (!out || !count) return ESP_ERR_INVALID_ARG;
    if (s_scan_pending) return ESP_ERR_INVALID_STATE;

    lock();
    size_t n = s_scan_count < max ? s_scan_count : max;
    memcpy(out, s_scan, n * sizeof(s_scan[0]));
    *count = n;
    unlock();
    return ESP_OK;
}

void love_net_ap_touch(void)
{
    s_ap_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(AP_DEFAULT_TIMEOUT_S * 1000);
}

// 由设置页/后台轮询:热点长时间无人访问时自动关闭,省电。
void love_net_poll(void)
{
    if (!s_inited || !s_ap_requested) return;
    if (s_sta_ssid[0] == '\0') return;   // 未配网时必须保留热点入口

    if (xTaskGetTickCount() > s_ap_deadline) {
        ESP_LOGI(TAG, "热点空闲超时,自动关闭");
        s_ap_requested = false;
        apply_mode();
    }
}
