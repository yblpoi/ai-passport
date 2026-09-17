// main/love_httpd.c —— 后台管理页与 REST 接口实现。
#include "love_httpd.h"

#include "love_admin_page.h"
#include "love_ble.h"
#include "love_date.h"
#include "love_net.h"
#include "love_store.h"
#include "love_time.h"
#include "love_web_assets.h"

#include "cJSON.h"
#include "bsp_battery.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "love_httpd";

#define BODY_MAX 3072
#define SCAN_WAIT_MS 7000

static httpd_handle_t s_server;
static love_httpd_changed_cb_t s_changed_cb;
// 最近一次请求的时刻(esp_timer 微秒)。两个出口 send_blob() 与 send_json() 都记一笔,
// 所以每个处理器都被覆盖到,不必在十几处各写一行。0 = 还没来过请求。
static volatile int64_t s_last_request_us;

// 每个响应都经过这里或 send_json():网页还在被人用时,应用据此推迟自动深睡眠。
static void note_client_activity(void)
{
    s_last_request_us = esp_timer_get_time();
}

uint32_t love_httpd_client_idle_seconds(void)
{
    const int64_t last = s_last_request_us;
    if (last == 0) return UINT32_MAX;
    const int64_t idle_us = esp_timer_get_time() - last;
    if (idle_us <= 0) return 0;
    return (uint32_t)(idle_us / 1000000);
}

void love_httpd_set_changed_cb(love_httpd_changed_cb_t cb)
{
    s_changed_cb = cb;
}

static void notify_changed(void)
{
    if (s_changed_cb) s_changed_cb();
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root, const char *status)
{
    note_client_activity();
    // root 可能是 NULL:state_to_json() 里的整份配置是堆上要来的,要不到就直接报 500,
    // 别把 NULL 递给 cJSON。
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json");
        return ESP_FAIL;
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    if (status) httpd_resp_set_status(req, status);
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message);
    return send_json(req, root, status);
}

/* ---------- base64(把自定义头像原样回给网页画缩略图) ---------- */

static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 定长输入(800 字节 -> 1068 字符)写进静态缓冲,避免每次请求动态分配。
// 返回值在下次调用前有效;调用方拿到后应立即拷走(cJSON 会拷贝)。
#define B64_BUF_MAX 1200
static const char *base64_encode(const uint8_t *data, size_t len)
{
    static char out[B64_BUF_MAX];
    out[0] = '\0';
    if (!data || len == 0) return out;
    if (((len + 2) / 3) * 4 + 1 > sizeof(out)) return out;

    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const size_t remain = len - i;
        uint32_t v = (uint32_t)data[i] << 16;
        if (remain > 1) v |= (uint32_t)data[i + 1] << 8;
        if (remain > 2) v |= (uint32_t)data[i + 2];

        out[o++] = B64_ALPHABET[(v >> 18) & 0x3F];
        out[o++] = B64_ALPHABET[(v >> 12) & 0x3F];
        out[o++] = (remain > 1) ? B64_ALPHABET[(v >> 6) & 0x3F] : '=';
        out[o++] = (remain > 2) ? B64_ALPHABET[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return out;
}

// 取查询串里的 slot。合法范围由调用方判断(返回 -1 表示没取到)。
static int query_slot(httpd_req_t *req)
{
    const size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen >= 48) return -1;
    char query[48];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return -1;
    char value[8];
    if (httpd_query_key_value(query, "slot", value, sizeof(value)) != ESP_OK) return -1;
    return atoi(value);
}

// 把请求体精确读满 len 字节。定长二进制(头像)与 JSON 两条读体路径共用这一段:
// httpd_req_recv 一次可能只给一部分,必须循环到读满或出错。
static esp_err_t recv_exact(httpd_req_t *req, char *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int ret = httpd_req_recv(req, buf + got, len - got);
        if (ret <= 0) return ESP_FAIL;
        got += (size_t)ret;
    }
    return ESP_OK;
}

// 请求体必须是定长二进制(头像不是 JSON,不走 read_json)。
static esp_err_t read_exact(httpd_req_t *req, uint8_t *buf, size_t expect)
{
    if (req->content_len != (int)expect) return ESP_ERR_INVALID_SIZE;
    return recv_exact(req, (char *)buf, expect);
}

// 读取并解析 JSON 请求体;失败时已回复错误响应。
static bool read_json(httpd_req_t *req, cJSON **out)
{
    *out = NULL;
    if (req->content_len == 0 || req->content_len >= BODY_MAX) {
        send_error(req, "400 Bad Request", "请求体为空或过大");
        return false;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) {
        send_error(req, "500 Internal Server Error", "内存不足");
        return false;
    }

    if (recv_exact(req, body, (size_t)req->content_len) != ESP_OK) {
        free(body);
        send_error(req, "400 Bad Request", "读取请求体失败");
        return false;
    }
    body[req->content_len] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        send_error(req, "400 Bad Request", "JSON 解析失败");
        return false;
    }
    *out = root;
    return true;
}

static cJSON *config_to_json(const love_config_t *cfg)
{
    char buf[16];
    cJSON *root = cJSON_CreateObject();

    love_date_format(cfg->start, buf, sizeof(buf));
    cJSON_AddStringToObject(root, "start", buf);
    // 自动熄屏秒数,0 = 常亮。后台页据此回填下拉框。
    cJSON_AddNumberToObject(root, "blankOff", cfg->blank_off_seconds);
    // 展示方式从 v4 起是每个事件自己的字段(见下面 events 里的 viewMode),
    // 不再有全局开关。这里也不再回 displayMode —— 后台页上的全局选择器已经去掉。
    cJSON_AddBoolToObject(root, "bleEnabled", cfg->ble_enabled != 0);

    cJSON *people = cJSON_AddArrayToObject(root, "people");
    for (size_t i = 0; i < LOVE_PERSON_MAX; i++) {
        cJSON *person = cJSON_CreateObject();
        cJSON_AddStringToObject(person, "name", cfg->people[i].name);
        cJSON_AddNumberToObject(person, "icon", cfg->people[i].icon);
        cJSON_AddItemToArray(people, person);
    }

    cJSON *events = cJSON_AddArrayToObject(root, "events");
    for (size_t i = 0; i < cfg->event_count && i < LOVE_EVENT_MAX; i++) {
        const love_event_t *event = &cfg->events[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", event->name);
        cJSON_AddNumberToObject(item, "icon", event->icon);
        cJSON_AddNumberToObject(item, "kind", event->kind);
        cJSON_AddStringToObject(item, "category", event->category);
        cJSON_AddNumberToObject(item, "viewMode", event->view_mode);
        if (event->kind == LOVE_EVENT_LUNAR) {
            // 农历事件用独立的月/日字段,不伪造一个公历日期
            cJSON_AddNumberToObject(item, "lunarMonth", event->date.month);
            cJSON_AddNumberToObject(item, "lunarDay", event->date.day);
        } else {
            love_date_format(event->date, buf, sizeof(buf));
            cJSON_AddStringToObject(item, "date", buf);
        }
        cJSON_AddItemToArray(events, item);
    }
    return root;
}

static cJSON *state_to_json(void)
{
    // 一份 love_config_t 有 1454 字节(v4 起最多 24 条事件)。httpd 任务的栈还要装
    // cJSON 与"配置变更时整屏重绘"的调用链,这类读一眼就丢的整份配置一律走堆 ——
    // 串口控制台任务就是因为同样的一份配置放在栈上而栈溢出的(实测)。
    love_config_t *cfg = malloc(sizeof(*cfg));
    if (!cfg) return NULL;
    love_store_load_config(cfg);

    love_time_state_t time_state;
    love_time_get(&time_state);

    love_net_status_t net;
    love_net_get_status(&net);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "deviceName", net.ap_ssid);

    // 真机右上角显示的电量，取不到时为 -1。
    cJSON_AddNumberToObject(root, "battery", bsp_battery_soc());

    cJSON_AddItemToObject(root, "config", config_to_json(cfg));

    cJSON *time = cJSON_AddObjectToObject(root, "time");
    char described[48] = { 0 };
    love_time_describe(&time_state, described, sizeof(described));
    cJSON_AddBoolToObject(time, "synced", time_state.holds);
    cJSON_AddNumberToObject(time, "epoch", (double)time_state.epoch_seconds);
    cJSON_AddStringToObject(time, "sourceText", love_time_src_text(time_state.source));
    cJSON_AddStringToObject(time, "text", described);

    cJSON *net_json = cJSON_AddObjectToObject(root, "net");
    cJSON_AddStringToObject(net_json, "state", net.state == LOVE_NET_CONNECTED ? "connected" :
                                             (net.state == LOVE_NET_CONNECTING ? "connecting" : "idle"));
    cJSON_AddStringToObject(net_json, "stateText", love_net_state_text(net.state));
    cJSON_AddBoolToObject(net_json, "ap", net.ap_active);
    // 手动关掉的热点不会自己回来(见 love_net.h),页面要把这件事说清楚。
    cJSON_AddBoolToObject(net_json, "apManualOff", net.ap_manual_off);
    cJSON_AddStringToObject(net_json, "ip", net.ip);
    cJSON_AddNumberToObject(net_json, "rssi", net.rssi);
    cJSON_AddStringToObject(net_json, "ssid", net.sta_ssid);
    cJSON_AddStringToObject(net_json, "apSsid", net.ap_ssid);
    cJSON_AddStringToObject(net_json, "apPass", net.ap_pass);
    cJSON_AddStringToObject(net_json, "url", net.site_url);
    cJSON_AddStringToObject(net_json, "lanUrl", net.lan_url);
    cJSON_AddBoolToObject(net_json, "hasCredentials", net.has_credentials);

    cJSON *icons = cJSON_AddArrayToObject(root, "icons");
    for (uint8_t i = 0; i < LOVE_ICON_TOTAL; i++) cJSON_AddItemToArray(icons, cJSON_CreateNumber(i));

    // 自定义头像槽位:已上传的把 4bpp 原始数据以 base64 回给网页,
    // 网页才能在选择器里画出缩略图(重开页面后也还在)。空槽位给空串。
    cJSON *avatars = cJSON_AddArrayToObject(root, "avatars");
    static uint8_t slot_data[LOVE_AVATAR_BYTES];
    for (uint8_t slot = 0; slot < LOVE_AVATAR_MAX; slot++) {
        if (love_store_load_avatar(slot, slot_data, sizeof(slot_data)) == LOVE_AVATAR_BYTES) {
            cJSON_AddItemToArray(avatars, cJSON_CreateString(base64_encode(slot_data,
                                                                          LOVE_AVATAR_BYTES)));
        } else {
            cJSON_AddItemToArray(avatars, cJSON_CreateString(""));
        }
    }
    free(cfg);
    return root;
}

// 改动落盘后的统一收尾:先让设备界面刷新,再把最新状态回给网页。
static esp_err_t finish(httpd_req_t *req)
{
    notify_changed();
    return send_json(req, state_to_json(), NULL);
}

// 每块只写 512 字节。这个值不能随便放大:send() 单次能写多少,取决于 lwIP 能不能
// 为这次写分配一块**连续**内存,而本机启动后堆只有二十 KB 上下。之前排障时正是
// 卡在这里——驱动发一帧要 ~1600 字节连续内存,分不到就发不出帧,客户端不 ACK,
// send() 一直阻塞到超时。块小,每次分配需求就小,和驱动抢连续内存时更从容;
// lwIP 仍会按 MSS 把这些小写合并成满段,线上效率不受影响。
#define PAGE_CHUNK_MAX 512

// cacheable=true 用于内容随固件固定的资源(底纹、图标):给一小时缓存。
// HTML/CSS/JS 不缓存,免得重新烧录后浏览器还按旧脚本发请求。
static esp_err_t send_blob(httpd_req_t *req, const char *type, const void *data,
                           size_t length, bool cacheable)
{
    note_client_activity();
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control",
                       cacheable ? "public, max-age=3600" : "no-store");

    const char *cursor = (const char *)data;
    for (size_t offset = 0; offset < length; offset += PAGE_CHUNK_MAX) {
        size_t chunk = length - offset;
        if (chunk > PAGE_CHUNK_MAX) chunk = PAGE_CHUNK_MAX;
        esp_err_t err = httpd_resp_send_chunk(req, cursor + offset, chunk);
        if (err != ESP_OK) {
            // 带上堆状况:这个失败几乎总是"连续内存不够",只看 EAGAIN 看不出原因。
            ESP_LOGE(TAG, "%s 在 %u/%u 字节处发送失败: %s(堆余 %u,最大连续块 %u)",
                     type, (unsigned)offset, (unsigned)length, esp_err_to_name(err),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            return err;
        }
    }
    return httpd_resp_send_chunk(req, NULL, 0);   // 结束分块传输
}

// 页面、样式与脚本各自一个请求,都只有几 KB。设备预览的字形用浏览器自己的
// 系统字体:内嵌那份像素字体子集要 122 KB,是页面的二十倍,得不偿失。
static esp_err_t handle_page(httpd_req_t *req)
{
    love_net_ap_touch();
    return send_blob(req, "text/html; charset=utf-8",
                     LOVE_ADMIN_HTML, LOVE_ADMIN_HTML_SIZE, false);
}

static esp_err_t handle_css(httpd_req_t *req)
{
    love_net_ap_touch();
    return send_blob(req, "text/css; charset=utf-8",
                     LOVE_ADMIN_CSS, LOVE_ADMIN_CSS_SIZE, false);
}

static esp_err_t handle_js(httpd_req_t *req)
{
    love_net_ap_touch();
    return send_blob(req, "application/javascript; charset=utf-8",
                     LOVE_ADMIN_JS, LOVE_ADMIN_JS_SIZE, false);
}

static esp_err_t handle_bg(httpd_req_t *req)
{
    love_net_ap_touch();
    return send_blob(req, "image/png", LOVE_WEB_BG_PNG, LOVE_WEB_BG_PNG_SIZE, true);
}

// /favicon.ico 与 /apple-touch-icon*.png —— 浏览器在解析页面时自己去取这两个路径,
// 没法用内联的 data URI 应答。三处都回同一张爱心,免得每次加载留下 404 警告。
static esp_err_t handle_page_icon(httpd_req_t *req)
{
    love_net_ap_touch();
    return send_blob(req, "image/png", LOVE_WEB_PAGE_ICON_PNG,
                     LOVE_WEB_PAGE_ICON_PNG_SIZE, true);
}

static esp_err_t handle_state(httpd_req_t *req)
{
    love_net_ap_touch();
    return send_json(req, state_to_json(), NULL);
}

// POST /api/avatar?slot=N
// 请求体是 LOVE_AVATAR_BYTES 字节的 4bpp 索引数据(索引指向 love_pixel_palette),
// 由后台网页用 canvas 压好再传,设备端不做图像处理。
static esp_err_t handle_avatar(httpd_req_t *req)
{
    love_net_ap_touch();

    const int slot = query_slot(req);
    if (slot < 0 || slot >= LOVE_AVATAR_MAX) {
        return send_error(req, "400 Bad Request", "slot 参数不合法");
    }

    static uint8_t data[LOVE_AVATAR_BYTES];
    if (read_exact(req, data, sizeof(data)) != ESP_OK) {
        return send_error(req, "400 Bad Request", "头像数据必须是 800 字节的 4bpp 数据");
    }

    if (love_store_save_avatar((uint8_t)slot, data) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "保存头像失败");
    }
    // 成功也留一行:上传是 800 字节的二进制 POST,失败了页面只弹一个 toast,
    // 设备侧要是也不说话,"没上传"和"上传成功"在日志里就分不出来。
    ESP_LOGI(TAG, "头像已保存: 槽位 %d", slot);
    return finish(req);
}

// POST /api/avatar/clear?slot=N —— 清掉槽位,退回内置图标。
static esp_err_t handle_avatar_clear(httpd_req_t *req)
{
    love_net_ap_touch();

    const int slot = query_slot(req);
    if (slot < 0 || slot >= LOVE_AVATAR_MAX) {
        return send_error(req, "400 Bad Request", "slot 参数不合法");
    }
    (void)love_store_clear_avatar((uint8_t)slot);
    return finish(req);
}

// 把网页传来的 JSON 叠到 cfg 上。字段缺失时**保留原值** —— 浏览器缓存的旧 admin.js
// 不带 category / viewMode / bleEnabled,一次保存就把用户分好的类或挑好的展示方式
// 抹掉是不能接受的。
static void apply_config_json(const cJSON *root, love_config_t *cfg)
{
    const cJSON *start = cJSON_GetObjectItem(root, "start");
    if (cJSON_IsString(start)) {
        love_date_t parsed;
        if (love_date_parse(start->valuestring, &parsed)) cfg->start = parsed;
    }

    // 只接受已知档位,别让网页写入任意秒数。
    const cJSON *blank = cJSON_GetObjectItem(root, "blankOff");
    if (cJSON_IsNumber(blank) && love_blank_off_valid((uint16_t)blank->valueint)) {
        cfg->blank_off_seconds = (uint16_t)blank->valueint;
    }

    // 蓝牙开关:字段缺失时保留原值 —— 浏览器缓存的旧 admin.js 不带这个字段,
    // 一次保存就把设置抹掉是不能接受的。
    const cJSON *ble = cJSON_GetObjectItem(root, "bleEnabled");
    if (cJSON_IsBool(ble)) cfg->ble_enabled = cJSON_IsTrue(ble) ? 1 : 0;

    const cJSON *people = cJSON_GetObjectItem(root, "people");
    if (cJSON_IsArray(people)) {
        size_t index = 0;
        const cJSON *person = NULL;
        cJSON_ArrayForEach(person, people) {
            if (index >= LOVE_PERSON_MAX) break;
            const cJSON *name = cJSON_GetObjectItem(person, "name");
            const cJSON *icon = cJSON_GetObjectItem(person, "icon");
            if (cJSON_IsString(name)) {
                snprintf(cfg->people[index].name, sizeof(cfg->people[index].name), "%s",
                         name->valuestring);
            }
            if (cJSON_IsNumber(icon) && icon->valueint >= 0 && icon->valueint < LOVE_ICON_TOTAL) {
                cfg->people[index].icon = (uint8_t)icon->valueint;
            }
            index++;
        }
    }

    const cJSON *events = cJSON_GetObjectItem(root, "events");
    if (cJSON_IsArray(events)) {
        uint8_t count = 0;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, events) {
            if (count >= LOVE_EVENT_MAX) break;
            const cJSON *name = cJSON_GetObjectItem(item, "name");
            const cJSON *icon = cJSON_GetObjectItem(item, "icon");
            const cJSON *kind = cJSON_GetObjectItem(item, "kind");
            const cJSON *date = cJSON_GetObjectItem(item, "date");
            const cJSON *lunar_month = cJSON_GetObjectItem(item, "lunarMonth");
            const cJSON *lunar_day = cJSON_GetObjectItem(item, "lunarDay");
            const cJSON *category = cJSON_GetObjectItem(item, "category");
            const cJSON *view = cJSON_GetObjectItem(item, "viewMode");

            love_event_t *event = &cfg->events[count];
            // 旧缓存页面不带 category / viewMode:按同下标保留老值,别把用户分好的类
            // 或挑好的展示方式抹掉。(旧页面的顺序也是它自己那份,同下标就是同一条。)
            char keep_category[LOVE_CATEGORY_MAX];
            love_utf8_copy(keep_category, sizeof(keep_category),
                           count < cfg->event_count ? cfg->events[count].category : "");
            const uint8_t keep_view = (count < cfg->event_count)
                                          ? cfg->events[count].view_mode
                                          : LOVE_EVENT_VIEW_LIST;
            memset(event, 0, sizeof(*event));
            if (cJSON_IsString(name)) {
                snprintf(event->name, sizeof(event->name), "%s", name->valuestring);
            }
            if (event->name[0] == '\0') snprintf(event->name, sizeof(event->name), "纪念日");
            love_utf8_copy(event->category, sizeof(event->category),
                           cJSON_IsString(category) ? category->valuestring : keep_category);
            event->view_mode = (cJSON_IsNumber(view) &&
                                (view->valueint == (int)LOVE_EVENT_VIEW_LIST ||
                                 view->valueint == (int)LOVE_EVENT_VIEW_PAGE))
                                   ? (uint8_t)view->valueint
                                   : keep_view;
            if (cJSON_IsNumber(icon) && icon->valueint >= 0 && icon->valueint < LOVE_ICON_TOTAL) {
                event->icon = (uint8_t)icon->valueint;
            }
            if (cJSON_IsNumber(kind) && kind->valueint == LOVE_EVENT_ONCE) {
                event->kind = LOVE_EVENT_ONCE;
            } else if (cJSON_IsNumber(kind) && kind->valueint == LOVE_EVENT_LUNAR) {
                event->kind = LOVE_EVENT_LUNAR;
            } else {
                event->kind = LOVE_EVENT_YEARLY;
            }

            if (event->kind == LOVE_EVENT_LUNAR) {
                // 农历事件不走公历日期字段:month/day 直接就是农历月日(day = 0 表示月末)。
                // 塞进 date 字符串会带一个没意义的年份,反而容易看错。
                const int month = cJSON_IsNumber(lunar_month) ? lunar_month->valueint : 1;
                const int day = cJSON_IsNumber(lunar_day) ? lunar_day->valueint : 1;
                event->date = (love_date_t){ cfg->start.year,
                                             (int8_t)((month >= 1 && month <= 12) ? month : 1),
                                             (int8_t)((day >= 0 && day <= 30) ? day : 1) };
            } else {
                love_date_t parsed;
                if (cJSON_IsString(date) && love_date_parse(date->valuestring, &parsed)) {
                    event->date = parsed;
                } else {
                    event->date = cfg->start;
                }
            }
            count++;
        }
        cfg->event_count = count;
    }

}

static esp_err_t handle_config(httpd_req_t *req)
{
    love_net_ap_touch();

    cJSON *root = NULL;
    if (!read_json(req, &root)) return ESP_FAIL;

    // 一份 love_config_t 是 1454 字节。这条路径上还要叠着 cJSON、保存、以及配置变更
    // 回调里的整屏重绘 —— 放栈上实测直接把 httpd 任务(6KB 栈)顶穿,所以走堆。
    love_config_t *cfg = malloc(sizeof(*cfg));
    if (!cfg) {
        cJSON_Delete(root);
        return send_error(req, "500 Internal Server Error", "内存不足");
    }
    love_store_load_config(cfg);
    apply_config_json(root, cfg);
    cJSON_Delete(root);

    esp_err_t err = love_store_save_config(cfg);
    free(cfg);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "保存配置失败");
    }

    // 改的是 2.4G Wi-Fi 名字以外的内容:立即刷新设备界面。
    return finish(req);
}

static esp_err_t handle_time(httpd_req_t *req)
{
    love_net_ap_touch();

    cJSON *root = NULL;
    if (!read_json(req, &root)) return ESP_FAIL;

    const cJSON *epoch = cJSON_GetObjectItem(root, "epoch");
    if (!cJSON_IsNumber(epoch) || epoch->valuedouble <= 0) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "缺少 epoch(秒)");
    }
    uint64_t value = (uint64_t)epoch->valuedouble;
    cJSON_Delete(root);

    esp_err_t err = love_time_set(value, LOVE_TIME_SRC_WEB);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "对时失败");
    }
    return finish(req);
}

static esp_err_t handle_scan(httpd_req_t *req)
{
    love_net_ap_touch();

    if (!love_net_scan_pending()) {
        esp_err_t err = love_net_scan_start();
        if (err != ESP_OK) {
            // 把真实原因写进日志:网页只看到一句笼统的 409,而这里的原因
            // 通常是"当前是纯 AP 模式,STA 接口不在场",光看响应看不出来。
            ESP_LOGW(TAG, "扫描启动失败: %s", esp_err_to_name(err));
            return send_error(req, "409 Conflict", "设备当前无法扫描(检查热点或网络状态)");
        }
    }

    // 扫描在 Wi-Fi 任务里完成,这里等待结果;超时如实返回空列表。
    for (int waited = 0; waited < SCAN_WAIT_MS && love_net_scan_pending(); waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    love_net_ap_t aps[LOVE_NET_SCAN_MAX];
    size_t count = 0;
    love_net_scan_results(aps, LOVE_NET_SCAN_MAX, &count);

    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(root, "aps");
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(item, "secure", aps[i].secure);
        cJSON_AddItemToArray(list, item);
    }
    return send_json(req, root, NULL);
}

static esp_err_t handle_wifi_save(httpd_req_t *req)
{
    love_net_ap_touch();

    cJSON *root = NULL;
    if (!read_json(req, &root)) return ESP_FAIL;

    const cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    const cJSON *pass = cJSON_GetObjectItem(root, "pass");
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "请填写 Wi-Fi 名称");
    }

    esp_err_t err = love_net_set_credentials(ssid->valuestring,
                                             cJSON_IsString(pass) ? pass->valuestring : "");
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "保存凭据失败");
    }
    return send_json(req, state_to_json(), NULL);
}

static esp_err_t handle_wifi_clear(httpd_req_t *req)
{
    love_net_ap_touch();
    esp_err_t err = love_net_forget();
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "清除凭据失败");
    }
    return send_json(req, state_to_json(), NULL);
}

static esp_err_t handle_ap(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (!read_json(req, &root)) return ESP_FAIL;

    const cJSON *on = cJSON_GetObjectItem(root, "on");
    bool enable = cJSON_IsTrue(on);
    cJSON_Delete(root);

    esp_err_t err = enable ? love_net_ap_start() : love_net_ap_stop();
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "热点操作失败");
    }
    return send_json(req, state_to_json(), NULL);
}

static const httpd_uri_t URIS[] = {
    { .uri = "/",              .method = HTTP_GET,  .handler = handle_page },
    { .uri = "/admin.css",     .method = HTTP_GET,  .handler = handle_css },
    { .uri = "/admin.js",      .method = HTTP_GET,  .handler = handle_js },
    { .uri = "/bg.png",        .method = HTTP_GET,  .handler = handle_bg },
    { .uri = "/favicon.ico",   .method = HTTP_GET,  .handler = handle_page_icon },
    { .uri = "/apple-touch-icon.png", .method = HTTP_GET, .handler = handle_page_icon },
    { .uri = "/apple-touch-icon-precomposed.png",
                               .method = HTTP_GET,  .handler = handle_page_icon },
    { .uri = "/api/state",     .method = HTTP_GET,  .handler = handle_state },
    { .uri = "/api/config",    .method = HTTP_POST, .handler = handle_config },
    { .uri = "/api/time",      .method = HTTP_POST, .handler = handle_time },
    { .uri = "/api/scan",      .method = HTTP_GET,  .handler = handle_scan },
    { .uri = "/api/wifi",      .method = HTTP_POST, .handler = handle_wifi_save },
    { .uri = "/api/wifi/clear", .method = HTTP_POST, .handler = handle_wifi_clear },
    { .uri = "/api/avatar",    .method = HTTP_POST, .handler = handle_avatar },
    { .uri = "/api/avatar/clear", .method = HTTP_POST, .handler = handle_avatar_clear },
    { .uri = "/api/ap",        .method = HTTP_POST, .handler = handle_ap },
};

esp_err_t love_httpd_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = sizeof(URIS) / sizeof(URIS[0]);
    // 一个页面要取 HTML/CSS/JS/底纹,再算上 /api/state,浏览器是并发取的。
    // 原来只留 3 个会话,第 4 个连接进来就会触发 LRU 驱逐,把正在传输的响应掐掉。
    // 6 是浏览器的单站并发上限;再加 httpd 内部自留的 3 个共 9 个,不超过
    // LWIP_MAX_SOCKETS(10),不用改 lwIP 配置。
    config.max_open_sockets = 6;
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    // 发送超时放宽到 20 秒。客户端一旦读得慢,发送窗口就会短暂排不空;
    // 原来的 5 秒会让 httpd 把这种"慢但在推进"的情况判成失败
    // (日志 "httpd_sock_err: error in send : 11")并把响应掐断。
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 20;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    for (size_t i = 0; i < sizeof(URIS) / sizeof(URIS[0]); i++) {
        esp_err_t reg = httpd_register_uri_handler(s_server, &URIS[i]);
        if (reg != ESP_OK) {
            ESP_LOGE(TAG, "注册 %s 失败: %s", URIS[i].uri, esp_err_to_name(reg));
            love_httpd_stop();
            return reg;
        }
    }
    ESP_LOGI(TAG, "后台网页已就绪,访问 http://192.168.4.1");
    return ESP_OK;
}

void love_httpd_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
}
