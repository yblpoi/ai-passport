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

// 列表长度在两个头文件里各写了一份(理由见 love_net.h),对不上就是真错。
_Static_assert(LOVE_NET_SAVED_MAX == LOVE_WIFI_MAX,
               "love_net.h 的 LOVE_NET_SAVED_MAX 必须等于 love_store.h 的 LOVE_WIFI_MAX");

#define AP_SSID_PREFIX "LoveCount"
#define AP_DEFAULT_TIMEOUT_S 300

// 单个候选的连接超时。15 秒是实测出来的:正常路由器从 esp_wifi_connect() 到拿到 IP
// 在 1~3 秒,弱信号的角落最长见过 9 秒;再长就说明这个候选根本不在了,该换下一个。
#define CONNECT_TIMEOUT_MS 15000
// 一轮(所有候选都试过)失败后的冷却,指数退避到这个上限。
// 为什么要退避:断网时一轮要花掉"候选数 × 15 秒"的射频时间,不设冷却就是一直在
// 空转重试 —— 费电,而且串口会被"又没连上"刷屏(用户报的正是这个)。
#define SELECT_COOLDOWN_MIN_MS 30000
#define SELECT_COOLDOWN_MAX_MS 300000
// "与已连上的热点断开"之后的重连延迟:给路由器/手机热点一点恢复时间,别打成一团。
#define RESELECT_AFTER_DROP_MS 3000
// "整轮都没连上"这条日志的最小间隔。退避到 5 分钟一轮之后,这一条才是稳态输出;
// 长时间断网时它也就是 5 分钟一行。
#define ROUND_LOG_MIN_INTERVAL_MS 300000

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static SemaphoreHandle_t s_lock;

static bool s_inited;
static bool s_wifi_inited;
static bool s_wifi_started;
static bool s_ap_requested;
// 用户手动关掉过热点的闸。开着它时,任何"自动开热点"的路径都要让路:
// 开机自动开(见 love_net_init)与联网失败兜底(见 love_net_poll)。
// 只有"手动开热点"和"清除凭据"能把它放下,并且会写回 NVS(重启后仍然不自动开)。
static bool s_ap_manual_off;
static love_net_state_t s_state = LOVE_NET_OFF;
static char s_ip[16];
static int s_rssi = -127;
// 当前的目标/已连上的 SSID。改名为"目标"是因为它不再等于"唯一保存的那个":
// 一组凭据在 s_saved[] 里,这里只记"这一轮在连谁"(界面与网页显示的就是它)。
static char s_sta_ssid[LOVE_WIFI_SSID_MAX];

// 我们自己调 esp_wifi_disconnect() 之后的一小段时间:紧随其后的那条 DISCONNECTED
// 事件是在报告"我们刚才的动作",不是一次失败。窗口有界(2 秒),所以哪怕那次 disconnect
// 根本没产生事件,最多影响 2 秒内的判定,不会长期吞掉真实的失败。
#define SELF_DISCONNECT_QUIET_MS 2000

static love_net_ap_t s_scan[LOVE_NET_SCAN_MAX];
static size_t s_scan_count;
static volatile bool s_scan_pending;

// 凭据列表的互斥。**不用 s_lock**:那个锁被 love_net_get_status 在 LVGL 任务上取,
// 而这里要跨越 NVS 写(可能几十毫秒)与 esp_wifi 调用,拿它会把界面卡出可见的顿挫。
// 这把锁只保护"谁在改 s_saved[]",读写双方都只用极短的一段。
static SemaphoreHandle_t s_saved_lock;

/* ---------- 多热点选网状态 ---------- */
//
// **所有权**:下面这组状态(候选、已试位图、冷却、下次选网时刻)只由 love_net_poll
// 那一条路径读写 —— 它跑在应用的心跳任务上(1 秒一次)。别的事件回调与命令入口
// 一律只置一个 volatile 位(见下面的"请求位"),由 poll 统一消化。
//
// 为什么这么较真:Wi-Fi 事件在事件任务上,命令在控制台任务上,心跳又在一个任务上。
// 三处直接改同一个状态机就会出现"候选被试两遍""卡在 CONNECTING 不动"这类只在
// 现场偶发的问题,而且没有任何编译期保护。位标志都是单字节写,不存在撕裂。
static love_wifi_cred_t s_saved[LOVE_WIFI_MAX];
static size_t s_saved_count;

// 列表由控制台/网页任务增删、由 poll 读取。**改的那一侧加了互斥**(s_saved_lock),
// 因为两条改的路径可能同时来自不同任务(BLE 串口一边敲 wifi del、网页一边删一条),
// 而修改本身是多字搬运——两处并发就可能把一条被搬了一半的记录写进 NVS。
// 读的那一侧(选网、列列表)不加锁:最坏情况是读到一条"正在被搬动"的记录,表现为一次
// 注定失败的连接尝试或列表里一闪而过的错名字,15 秒后自愈,不会落到盘上。
// 真正需要严格互斥的只有 s_ip/s_rssi/s_state(它们被 get_status 从别的任务读),那些走 s_lock。

// 正在尝试的候选下标;-1 = 没在尝试(空闲、已连上、或这一轮已经结束)。
static int s_candidate = -1;
static TickType_t s_candidate_since;
// 本轮已经试过哪些候选(位图)。扫描挑中的那个也记进来,失败后按保存顺序补试其余。
static uint8_t s_tried;
// poll 看到它就启动新一轮选网;具体时刻由 s_select_after 决定(冷却/退避)。
static bool s_need_select;
static TickType_t s_select_after;
static uint32_t s_cooldown_ms;
static TickType_t s_round_log_at;

// —— 跨任务的请求位(只能由 poll 清) ——
// 一次连接尝试失败(立刻失败由 start_candidate 置,断开事件由事件任务置)。
static volatile bool s_candidate_failed;
// 一次"为选网而扫"的扫描已经出结果。
static volatile bool s_scan_ready;
// 一条已连上的连接掉了。
static volatile bool s_dropped;
// 已经取得 IP(连上了)。
static volatile bool s_got_ip;
// 别的任务改了凭据列表,请求重新选网(新增/删除)。
static volatile bool s_reselect_request;
// 这次扫描是"为选网而扫"。
static volatile bool s_scan_for_pick;
// 我们自己刚调过 esp_wifi_disconnect() 的截止时刻(见 SELF_DISCONNECT_QUIET_MS)。
static TickType_t s_self_disconnect_until;

static TickType_t s_ap_deadline;
static TickType_t s_sta_down_since;   // 0 = 当前是连着的(或未配网)

// 有凭据却连不上时,过这么久就自动把热点开起来兜底(见 love_net_poll)。
// 用户手动关过热点(s_ap_manual_off)时这条兜底不生效。
#define AP_FALLBACK_AFTER_MS 60000

static void lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

// 凭据列表的增删互斥(见 s_saved_lock 的说明)。只包住"改内存"这一段:
// NVS 写很慢(可能几十毫秒),不能连它一起包,否则另一条改凭据的路径要干等。
static void saved_lock(void)
{
    if (s_saved_lock) xSemaphoreTake(s_saved_lock, portMAX_DELAY);
}

static void saved_unlock(void)
{
    if (s_saved_lock) xSemaphoreGive(s_saved_lock);
}

// 热点 SSID 由 MAC 派生,避免所有设备都是同一个名字;密码则由 love_store **每台随机
// 生成一次并持久化**(见 love_store_load_ap_pass 的说明):SSID 是公开的,密码不能由
// 它推导出来 —— 否则任何看到 SSID 的人都能算出密码,再进没有任何鉴权的后台页。
// 密码显示在设备屏幕上,只有物理接触的人看得到。
static char s_ap_pass[9];   // 初始化时读/生成一次,之后只读缓存

static void build_ap_identity(char *ssid, size_t ssid_size, char *pass, size_t pass_size)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ssid, ssid_size, "%s-%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
    // 读不到 NVS(首次初始化失败等)时退回按 MAC 派生:宁可密码可推导,也不留一个空密码
    // 的空热点。s_ap_pass 为空只可能是这一种情况。
    if (s_ap_pass[0] != '\0') {
        snprintf(pass, pass_size, "%s", s_ap_pass);
    } else {
        snprintf(pass, pass_size, "love%02x%02x", mac[4], mac[5]);
    }
}

static void apply_mode(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (s_ap_requested) {
        // 热点开着时一律用 APSTA,即使还没配网。
        //
        // 纯 AP 模式下 STA 接口不在场,esp_wifi_scan_start() 会直接失败
        // (实测返回 ESP_FAIL),而后台的"扫描附近 Wi-Fi"与开机时的"扫描挑最强的
        // 已知热点"都靠它 —— 最需要它的场景恰好用不了。STA 这边没有凭据时不会连接
        // (select_round_begin() 见 s_saved_count 为 0 即返回),代价只是扫描那 1~2 秒
        // 射频会离开服务信道,已连上的客户端可能感知到一次短暂中断。
        mode = WIFI_MODE_APSTA;
    } else if (s_saved_count > 0) {
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

// 本轮还有没有没试过的候选?有就返回下标,没有返回 -1。
static int next_untried(void)
{
    for (size_t i = 0; i < s_saved_count; i++) {
        if ((s_tried & (1u << i)) == 0) return (int)i;
    }
    return -1;
}

// 开始连接第 index 个候选。
static void start_candidate(size_t index)
{
    if (index >= s_saved_count) return;

    const love_wifi_cred_t *cred = &s_saved[index];
    s_candidate = (int)index;
    s_candidate_since = xTaskGetTickCount();
    s_candidate_failed = false;
    s_tried |= (uint8_t)(1u << index);

    snprintf(s_sta_ssid, sizeof(s_sta_ssid), "%s", cred->ssid);

    wifi_config_t config = { 0 };
    memcpy(config.sta.ssid, cred->ssid, strlen(cred->ssid));
    memcpy(config.sta.password, cred->pass, strlen(cred->pass));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    // 这一句的返回值必须看:一旦没写进去(密码长度不合法、驱动没就绪),接下来
    // esp_wifi_connect() 用的还是上一轮的配置、甚至是空配置 —— assoc 必然失败,
    // 而现场只剩一条语焉不详的 reason。这是"没有线索的失败",最贵的一种。
    const esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "候选 \"%s\":写入凭据失败(%s),这次连接用的不是它",
                 cred->ssid, esp_err_to_name(cfg_err));
    }

    s_state = LOVE_NET_CONNECTING;
    const esp_err_t err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_CONN) {
        // 唯一被容忍的一种:它的意思是"驱动已经在连了",这次调用等于没发起 ——
        // 本候选之后不会再有任何事件,只能等 poll 的 15 秒超时。poll 换候选前先
        // disconnect() 正是为了避开它,所以它一出现就是"这一轮白跑一个候选"的线索。
        ESP_LOGW(TAG, "候选 \"%s\":连接没能发起,驱动仍在连接中", cred->ssid);
    } else if (err != ESP_OK) {
        // 连"发起连接"都失败(驱动还没就绪等),当作这个候选失败,交给 poll 推进 ——
        // 这里不能递归去试下一个:本函数可能跑在事件回调的栈上。
        ESP_LOGW(TAG, "候选 \"%s\":发起连接失败(%s)", cred->ssid, esp_err_to_name(err));
        s_candidate_failed = true;
    }
}

// 一轮开始:先扫描。扫到已保存的热点就直连其中信号最强的那个 —— 一次成功,
// 不用逐个候选各等 15 秒;扫不到(包括对方的 SSID 是隐藏的)再按保存顺序逐个试。
static void select_round_begin(void)
{
    if (s_saved_count == 0) {
        s_state = LOVE_NET_IDLE;
        s_need_select = false;
        return;
    }
    if (!s_wifi_started) return;
    // 已经有一次扫描在飞(例如网页刚点了"扫描"):等它回来,poll 下一秒再走这里。
    if (s_scan_pending) return;

    s_tried = 0;
    s_candidate = -1;
    s_candidate_failed = false;

    if (love_net_scan_start() == ESP_OK) {
        s_scan_for_pick = true;
        ESP_LOGI(TAG, "选网:扫描附近的已知热点(%u 个候选)",
                 (unsigned)s_saved_count);
        return;
    }

    // 扫不了(例如 STA 接口不在场)也得连:按保存顺序试第一个。
    ESP_LOGI(TAG, "选网:无法扫描,按保存顺序尝试 %u 个候选", (unsigned)s_saved_count);
    start_candidate(0);
}

// 扫描结果回来了:在已保存的候选里挑信号最强的。
static void pick_from_scan(void)
{
    int best = -1;
    int best_rssi = -127;
    for (size_t i = 0; i < s_saved_count; i++) {
        for (size_t j = 0; j < s_scan_count; j++) {
            if (strcmp(s_saved[i].ssid, s_scan[j].ssid) != 0) continue;
            if ((int)s_scan[j].rssi > best_rssi) {
                best_rssi = s_scan[j].rssi;
                best = (int)i;
            }
        }
    }

    if (best < 0) {
        ESP_LOGI(TAG, "选网:附近没有已保存的热点,按保存顺序尝试");
        start_candidate(0);
        return;
    }

    ESP_LOGI(TAG, "选网:连接信号最强的 \"%s\"(%d dBm)", s_saved[best].ssid, best_rssi);
    start_candidate((size_t)best);
}

// 当前候选失败了:换下一个;都试过就进入冷却。
static void advance_candidate(void)
{
    s_candidate = -1;
    s_candidate_failed = false;

    const int next = next_untried();
    if (next >= 0) {
        start_candidate((size_t)next);
        return;
    }

    s_state = LOVE_NET_FAILED;
    s_cooldown_ms = s_cooldown_ms
                  ? (s_cooldown_ms * 2 > SELECT_COOLDOWN_MAX_MS
                         ? SELECT_COOLDOWN_MAX_MS : s_cooldown_ms * 2)
                  : SELECT_COOLDOWN_MIN_MS;

    const TickType_t now = xTaskGetTickCount();
    if (s_round_log_at == 0 ||
        (now - s_round_log_at) >= pdMS_TO_TICKS(ROUND_LOG_MIN_INTERVAL_MS)) {
        ESP_LOGW(TAG, "已保存的 %u 个热点都没连上,%u 秒后再试",
                 (unsigned)s_saved_count, (unsigned)(s_cooldown_ms / 1000));
        s_round_log_at = now;
    }

    s_need_select = true;
    s_select_after = now + pdMS_TO_TICKS(s_cooldown_ms);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        // 真正的连接节奏由 love_net_poll 驱动(见那里的说明),这里只把"该选网了"立起来。
        s_reselect_request = s_saved_count > 0;
        break;
    case WIFI_EVENT_STA_CONNECTED:
        s_state = LOVE_NET_CONNECTING;
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *event = data;
        const int reason = event ? event->reason : -1;
        const bool was_connected = (s_state == LOVE_NET_CONNECTED);

        lock();
        s_ip[0] = '\0';
        s_rssi = -127;
        s_state = LOVE_NET_FAILED;
        unlock();

        if (was_connected) {
            // 只报"连着的掉了"这一种:它是用户能感知的事件(页面会变、天数可能变未同步)。
            ESP_LOGW(TAG, "与 \"%s\" 的连接断开(原因 %d),稍后重新选网", s_sta_ssid, reason);
            s_dropped = true;
        } else if ((int32_t)(xTaskGetTickCount() - s_self_disconnect_until) < 0) {
            // 刚才是我们自己 disconnect() 的(超时换候选/改凭据):这条事件报告的是
            // 我们自己的动作,不是候选失败 —— 记成失败会把下一个候选也一起跳掉。
        } else {
            // 一次连接尝试失败:不在这里重连、也不在这里推进状态机 —— 这正是原来
            // "秒级重连 + 每条一条 WARN"的刷屏来源。但 **reason 必须留下**:它是唯一
            // 能区分"找不到 AP(201)""认证失败(202)""关联失败(203)""握手超时(15/204)"
            // 的证据,而这条事件过去之后就再也拿不到了。
            // 用 info 而不是 warn,频率与"换候选"同阶:每轮每条候选最多一行,
            // 轮与轮之间至少隔 30 秒(退避后更长),不会回到按秒刷屏。
            ESP_LOGI(TAG, "候选 \"%s\" 连不上(原因 %d)", s_sta_ssid, reason);
            s_candidate_failed = true;
        }
        break;
    }
    case WIFI_EVENT_SCAN_DONE: {
        // 静态缓冲:扫描同一时刻只会有一份,避免占用事件任务的栈。
        static wifi_ap_record_t records[LOVE_NET_SCAN_MAX];
        memset(records, 0, sizeof(records));
        uint16_t count = LOVE_NET_SCAN_MAX;
        lock();
        // 条件里只看 get_ap_records 的返回值。它同时是**释放驱动那份扫描结果内存**的
        // 调用,而 get_ap_num 只是把一个我们从没读过的总数写出来 —— 原先把它串在
        // `&&` 前面,它一旦失败就会连记录一起跳过,驱动那 1~3KB 扫描结果也就没人释放。
        if (esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
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
        const bool for_pick = s_scan_for_pick;
        s_scan_for_pick = false;
        s_scan_pending = false;
        unlock();
        // 只记"结果到了",挑哪一个由 poll 决定(选网状态机是它的,见上面的所有权说明)。
        if (for_pick) s_scan_ready = true;
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

    // 连上了:让 poll 收尾(退避清零、本轮结束)——选网状态只归它写。
    s_got_ip = true;
    ESP_LOGI(TAG, "已联网并取得 IP: %s", ip);
    love_time_sntp_start();
}

esp_err_t love_net_init(void)
{
    if (s_inited) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_saved_lock = xSemaphoreCreateMutex();
    if (!s_saved_lock) return ESP_ERR_NO_MEM;

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
    // 不省电。默认的 WIFI_PS_MIN_MODEM 会让射频按 DTIM 节拍才收发,后台网页有
    // 21 万字节要传,实测吞吐掉到 3KB/s 量级,发送窗口打不开后 httpd 直接报
    // "error in send : 11"(EAGAIN)并中断响应。设备是常电桌面摆件,省这点电不值当。
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // 热点密码:每台随机生成一次并落盘,必须在 start_ap_profile() 之前读好
    // (老设备这里会顺手生成 + 写入,所以它的密码会从 MAC 推导值变成随机值)。
    if (love_store_load_ap_pass(s_ap_pass, sizeof(s_ap_pass)) != ESP_OK) {
        ESP_LOGW(TAG, "读取热点密码失败,本次按 MAC 派生");
        s_ap_pass[0] = '\0';
    }

    start_ap_profile();

    // 凭据列表:一次读进内存,之后由本模块持有(NVS 只做持久化)。读的那一刻会
    // 顺手把老固件的单条记录搬进列表格式(见 love_store_load_wifi_list)。
    s_saved_count = love_store_load_wifi_list(s_saved, LOVE_WIFI_MAX);
    const bool has_creds = s_saved_count > 0;

    // 先把"用户手动关过热点"这一意图读回来:它决定下面要不要自动开热点,
    // 也决定联网失败时还要不要兜底(见 love_net_poll)。
    s_ap_manual_off = love_store_load_ap_off();
    if (!has_creds) {
        s_sta_ssid[0] = '\0';
        // 没配过网:开机就开热点,否则用户没有任何入口进后台。
        // 例外是用户上次在设置页手动把它关了 —— 那是明确的意图,不再自动开;
        // 想找回来后仍旧是设置页那一个按钮(见 love_app.c 的 ACT_AP_TOGGLE)。
        s_ap_requested = !s_ap_manual_off;
    }
    love_net_ap_touch();

    apply_mode();
    s_inited = true;
    // 有凭据就交给 poll 起第一轮选网(扫描 → 挑最强的那个;见 love_net_poll)。
    s_reselect_request = has_creds;
    s_state = has_creds ? LOVE_NET_CONNECTING : LOVE_NET_IDLE;
    ESP_LOGI(TAG, "Wi-Fi 已启动(已配网=%d,已保存 %u 个热点)", (int)has_creds,
             (unsigned)s_saved_count);
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
    if (s_saved_lock) {
        vSemaphoreDelete(s_saved_lock);
        s_saved_lock = NULL;
    }
    s_ap_requested = false;
    s_inited = false;
    s_state = LOVE_NET_OFF;
    s_ip[0] = '\0';

    // 选网状态一并复位:stop/start 可能发生在同一趟启动里(深睡回滚会走
    // love_app_stop → love_net_deinit → 再 start),留着上一轮的候选/已试位图/
    // 冷却时间会让新一轮从中间开始,甚至一直等一个早过期的 s_select_after。
    s_candidate = -1;
    s_candidate_failed = false;
    s_scan_ready = false;
    s_dropped = false;
    s_got_ip = false;
    s_reselect_request = false;
    s_scan_for_pick = false;
    s_need_select = false;
    s_tried = 0;
    s_cooldown_ms = 0;
    s_scan_pending = false;
    s_scan_count = 0;
}

esp_err_t love_net_ap_start(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    // 手动打开 = 撤销那道闸,并写回 NVS:用户要热点这件事重启后也成立。
    // 只在闸真的关着时写,免得每次启动流程(wifi clear 也走这里)都动一次 NVS。
    if (s_ap_manual_off) {
        s_ap_manual_off = false;
        (void)love_store_save_ap_off(false);
    }
    s_ap_requested = true;
    love_net_ap_touch();
    apply_mode();
    return ESP_OK;
}

esp_err_t love_net_ap_stop(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    s_ap_requested = false;
    // 记下"这一下是用户手动关的":两个自动开热点的路径都要让路,否则联网失败满
    // 60 秒热点又自己回来了(这正是用户报的现象)。写回 NVS,重启/深睡醒来也不再自动开。
    //
    // 空闲超时那次自动关(s_state 已联网、超 AP_DEFAULT_TIMEOUT_S)**不设**这道闸 ——
    // 那是设备自己的省电行为,之后联网再失败时仍然需要兜底入口。
    if (!s_ap_manual_off) {
        s_ap_manual_off = true;
        (void)love_store_save_ap_off(true);
    }
    apply_mode();
    return ESP_OK;
}

// 新凭据写进列表:同名就地更新密码(保留原来的尝试顺序),新名字追加在后面。
static esp_err_t list_upsert(const char *ssid, const char *pass)
{
    for (size_t i = 0; i < s_saved_count; i++) {
        if (strcmp(s_saved[i].ssid, ssid) != 0) continue;
        if (strcmp(s_saved[i].pass, pass) == 0) return ESP_OK;   // 一个字都没变
        snprintf(s_saved[i].pass, sizeof(s_saved[i].pass), "%s", pass);
        return love_store_save_wifi_list(s_saved, s_saved_count);
    }

    if (s_saved_count >= LOVE_WIFI_MAX) return ESP_ERR_INVALID_STATE;   // 列表满了
    snprintf(s_saved[s_saved_count].ssid, sizeof(s_saved[0].ssid), "%s", ssid);
    snprintf(s_saved[s_saved_count].pass, sizeof(s_saved[0].pass), "%s", pass);
    s_saved_count++;
    const esp_err_t err = love_store_save_wifi_list(s_saved, s_saved_count);
    if (err != ESP_OK) s_saved_count--;    // 没落盘就不要留在内存里,否则重启后"少一个"
    return err;
}

esp_err_t love_net_set_credentials(const char *ssid, const char *pass)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!pass) pass = "";
    if (strlen(ssid) >= LOVE_WIFI_SSID_MAX || strlen(pass) >= LOVE_WIFI_PASS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    // 是不是在改"当前这条":是的话必须断开重连(改了密码就等于换了凭据),
    // 不是的话就让现有连接留着 —— 用户加一个备用热点,不该把他现在这个踢掉。
    const bool is_current = (strcmp(s_sta_ssid, ssid) == 0);

    // 改 + 写盘整段持锁:两条改凭据的路径(串口控制台与网页)来自不同任务,
    // 并发时可能把一条被搬了一半的记录写进 NVS —— 那是会留到重启后的坏数据。
    saved_lock();
    const esp_err_t err = list_upsert(ssid, pass);
    saved_unlock();
    if (err != ESP_OK) return err;

    apply_mode();
    s_reselect_request = true;
    if (s_wifi_started && is_current) {
        // 断开会让 poll 看到 s_dropped,但这里已经明确要重选了,直接重来一轮更干脆。
        s_self_disconnect_until = xTaskGetTickCount() + pdMS_TO_TICKS(SELF_DISCONNECT_QUIET_MS);
        esp_wifi_disconnect();
    }
    return ESP_OK;
}

// 从列表里删掉一条(按 SSID)。找不到返回 ESP_ERR_NOT_FOUND。
esp_err_t love_net_forget_ssid(const char *ssid)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    saved_lock();
    size_t index = s_saved_count;
    for (size_t i = 0; i < s_saved_count; i++) {
        if (strcmp(s_saved[i].ssid, ssid) == 0) { index = i; break; }
    }
    if (index == s_saved_count) {
        saved_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    for (size_t i = index + 1; i < s_saved_count; i++) s_saved[i - 1] = s_saved[i];
    s_saved_count--;
    memset(&s_saved[s_saved_count], 0, sizeof(s_saved[0]));

    // 删空了:等于从没配过网 —— 交给 forget(),它连 NVS 里的列表一起清掉并开热点,
    // 否则设备会停在"没有任何入口"的状态(局域网连不上、热点也不开)。
    if (s_saved_count == 0) {
        saved_unlock();
        return love_net_forget();
    }

    const esp_err_t err = love_store_save_wifi_list(s_saved, s_saved_count);
    saved_unlock();
    if (err != ESP_OK) return err;

    const bool was_current = (strcmp(s_sta_ssid, ssid) == 0);
    apply_mode();
    s_reselect_request = true;
    if (s_wifi_started && was_current) {
        s_self_disconnect_until = xTaskGetTickCount() + pdMS_TO_TICKS(SELF_DISCONNECT_QUIET_MS);
        esp_wifi_disconnect();
    }
    return ESP_OK;
}

// 已保存的列表(按保存顺序)。供控制台与网页列出/删除;current 标记正在连的那条。
size_t love_net_saved_list(love_net_saved_t *out, size_t max)
{
    if (!out || max == 0) return 0;
    memset(out, 0, max * sizeof(out[0]));

    saved_lock();
    const size_t count = s_saved_count < max ? s_saved_count : max;
    for (size_t i = 0; i < count; i++) {
        // 用精度限制长度而不是裸 %s:编译器没法证明 s_saved[i].ssid 以 NUL 结尾
        // (它只看到一片下标可变的 char 数组),会按最坏情况报 -Wformat-truncation。
        snprintf(out[i].ssid, sizeof(out[i].ssid), "%.*s",
                 (int)sizeof(out[i].ssid) - 1, s_saved[i].ssid);
        out[i].current = (strcmp(s_sta_ssid, s_saved[i].ssid) == 0);
    }
    saved_unlock();
    return count;
}

esp_err_t love_net_forget(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    (void)love_store_clear_wifi();
    saved_lock();
    s_saved_count = 0;
    memset(s_saved, 0, sizeof(s_saved));
    saved_unlock();
    s_sta_ssid[0] = '\0';
    s_ip[0] = '\0';
    s_state = LOVE_NET_IDLE;
    s_reselect_request = false;
    if (s_wifi_started) esp_wifi_disconnect();

    // 清除凭据后必须留下配网入口。走 love_net_ap_start() 而不是自己置位:
    // 它同时会撤销"用户手动关过热点"的闸 —— 清凭据本身就是一次"我要重新配网"的
    // 明确意图,这时候再挡着不开热点,用户就真的没有入口了。
    return love_net_ap_start();
}

const char *love_net_state_text(love_net_state_t state)
{
    switch (state) {
    case LOVE_NET_OFF:        return "未启动";
    case LOVE_NET_IDLE:       return "未联网";
    case LOVE_NET_CONNECTING: return "连接中";
    case LOVE_NET_CONNECTED:  return "已联网";
    case LOVE_NET_FAILED:     return "连接失败";
    default:                  return "未知";
    }
}

void love_net_get_status(love_net_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    lock();
    out->state = s_state;
    out->ap_active = s_ap_requested;
    out->ap_manual_off = s_ap_manual_off;
    snprintf(out->ip, sizeof(out->ip), "%s", s_ip);
    out->rssi = s_rssi;
    snprintf(out->sta_ssid, sizeof(out->sta_ssid), "%s", s_sta_ssid);
    out->saved_count = s_saved_count;
    unlock();

    // "配过网"看的是列表,不是当前在连的那条:断网重选期间 sta_ssid 可能还空着,
    // 但设备明明配过网 —— 拿它判断会让"开机自动开热点"这类分支做错决定。
    out->has_credentials = out->saved_count > 0;
    build_ap_identity(out->ap_ssid, sizeof(out->ap_ssid),
                      out->ap_pass, sizeof(out->ap_pass));
    // 两个地址都给网页,由它分别显示:热点开着时 192.168.4.1 可用,但热点一旦
    // 空闲关闭(见 love_net_poll)就只剩设备在局域网里的地址,只报其中一个
    // 总有一半时间是错的。
    //
    // lan_url 用的是 s_ip —— 也就是 DHCP 实际分到的地址,每次取状态都重新拼,
    // 不要改成任何写死的地址。租约变化后网页下次轮询就会跟着更新。
    if (out->ap_active) {
        snprintf(out->site_url, sizeof(out->site_url), "http://192.168.4.1");
    }
    if (out->state == LOVE_NET_CONNECTED && out->ip[0] != '\0') {
        snprintf(out->lan_url, sizeof(out->lan_url), "http://%s", out->ip);
    }
}

esp_err_t love_net_scan_start(void)
{
    if (!s_inited || !s_wifi_started) return ESP_ERR_INVALID_STATE;
    if (s_scan_pending) return ESP_ERR_INVALID_STATE;
    // 选网那次扫描的结果还没被 poll 消化(s_scan_ready 到下一次心跳之间最多 1 秒):
    // 这时候放行外部扫描,启动时的 s_scan_count = 0 会把它盖掉,选网就"刚扫到却说
    // 附近没有已保存的热点"。宁可让网页等一秒重试,也不要把结果换掉。
    if (s_scan_ready) return ESP_ERR_INVALID_STATE;

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

// 由应用的心跳调用(1 秒一次):多热点选网的全部推进 + 热点兜底与空闲关闭。
//
// 为什么连接节奏放在这里,而不是像原先那样在 STA_DISCONNECTED 回调里立刻
// esp_wifi_connect():那个写法在"配过网但热点不在"时会以每秒一次的频率重连并
// 各打一条 WARN,串口被刷屏(用户报的就是这个)。放到 1 秒心跳上之后,失败不再
// 重连,日志也只跟着"每条候选一次尝试"走 —— 每次尝试至多一行(失败原因,或
// "一个事件都没来"的超时),整轮再一行汇总,不再按秒刷屏。
void love_net_poll(void)
{
    if (!s_inited) return;

    const TickType_t now = xTaskGetTickCount();

    /* ---- 消化别的任务投来的请求位 ---- */
    if (s_got_ip) {
        s_got_ip = false;
        s_dropped = false;
        s_candidate = -1;
        s_candidate_failed = false;
        s_need_select = false;
        s_cooldown_ms = 0;
        s_round_log_at = 0;      // 下次断网时重新从"第一次失败"开始报
    }
    if (s_dropped) {
        // 连着的掉了:歇 3 秒再重选,别跟路由器的重启撞在一起。
        s_dropped = false;
        s_candidate = -1;
        s_candidate_failed = false;
        s_cooldown_ms = 0;       // 掉线不是"连不上",退避从头算
        s_need_select = s_saved_count > 0;
        s_select_after = now + pdMS_TO_TICKS(RESELECT_AFTER_DROP_MS);
    }
    if (s_reselect_request) {
        // 凭据变了(新增/删除):立刻重来一轮,不等冷却。
        s_reselect_request = false;
        s_candidate = -1;
        s_candidate_failed = false;
        s_cooldown_ms = 0;
        s_need_select = s_saved_count > 0;
        s_select_after = now;
    }
    if (s_scan_ready) {
        s_scan_ready = false;
        pick_from_scan();
    }

    /* ---- 推进当前候选 / 开始新一轮 ---- */
    const bool mid_attempt = (s_candidate >= 0);
    if (mid_attempt && s_state != LOVE_NET_CONNECTED &&
        (s_candidate_failed ||
         (now - s_candidate_since) > pdMS_TO_TICKS(CONNECT_TIMEOUT_MS))) {
        // 换候选之前先把这一次收掉:驱动可能还停在"正在连接"上(它自己的超时比我们长),
        // 此时下一次 esp_wifi_connect() 会返回 ESP_ERR_WIFI_CONN —— 那个错误在
        // start_candidate 里被当成"已经在连了"容忍掉(只留一条 WARN),于是新候选的
        // 配置写进去了、连接却没发起,一整轮会白跑。disconnect() 在没有连接尝试时是无害的。
        if (s_wifi_started) {
            s_self_disconnect_until = now + pdMS_TO_TICKS(SELF_DISCONNECT_QUIET_MS);
            esp_wifi_disconnect();
        }
        // 超时收尾与"事件报告失败"在串口上必须分得开:上面那条 reason 日志只在真有事件
        // 时出现,这里对应的是"15 秒里一个事件都没来"(连接根本没发起、或者驱动卡住)。
        // 没有这一行,两种现场的日志长得一模一样。
        if (!s_candidate_failed) {
            ESP_LOGI(TAG, "候选 \"%s\":%d 秒内没收到任何连接事件,换下一个", s_sta_ssid,
                     CONNECT_TIMEOUT_MS / 1000);
        }
        advance_candidate();
    } else if (!mid_attempt && s_need_select && s_saved_count > 0 &&
               s_state != LOVE_NET_CONNECTED &&
               (int32_t)(now - s_select_after) >= 0) {
        select_round_begin();
    }

    // 联网失败兜底:配过网却连不上时,自动把热点开起来当入口。
    // 不这样做的话那种状态是"两头进不去"——配过网的设备开机不开热点(见 love_net_init),
    // 局域网地址又不存在,用户只剩 USB 一条路。实测遇到过:手机热点一关,后台就再也进不去。
    //
    // 唯一的例外是用户手动关过热点(s_ap_manual_off 为真):那说明"进不去"是他自己选的,
    // 再自动开回来就是噪音。这时兜底的计时照常累加 —— 一旦用户手动开一次热点,闸就撤销了。
    //
    // 计时只在"没在尝试连接"的空档里走:一轮候选要花掉十几到几十秒,过程中切到
    // APSTA 会把正在进行的连接打断(esp_wifi_set_mode 会重启射频),那等于白试一轮。
    const bool mid_round = (s_candidate >= 0);
    if (s_saved_count > 0 && s_state != LOVE_NET_CONNECTED && !mid_round) {
        if (s_sta_down_since == 0) {
            s_sta_down_since = now;
        } else if (!s_ap_requested && !s_ap_manual_off &&
                   (now - s_sta_down_since) > pdMS_TO_TICKS(AP_FALLBACK_AFTER_MS)) {
            ESP_LOGW(TAG, "联网失败超过 %d 秒,自动打开热点作为入口",
                     AP_FALLBACK_AFTER_MS / 1000);
            s_ap_requested = true;
            apply_mode();
        }
    } else {
        s_sta_down_since = 0;
    }

    if (!s_ap_requested) return;
    // 未配网时热点是唯一入口,不按空闲超时关它(手动关是允许的,见上面的闸)。
    if (s_saved_count == 0) return;

    // 联不上网时热点是唯一入口,不按空闲关闭;连上之后让它正常计时关闭。
    if (s_state != LOVE_NET_CONNECTED) {
        love_net_ap_touch();
        return;
    }

    if (xTaskGetTickCount() > s_ap_deadline) {
        ESP_LOGI(TAG, "热点空闲超时,自动关闭");
        s_ap_requested = false;
        apply_mode();
    }
}
