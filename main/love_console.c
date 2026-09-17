// main/love_console.c —— 命令行控制台,USB 与 BLE 共用一套命令(见 love_console.h)。
//
// 命令表 COMMANDS 是唯一真源:USB 侧把它逐条注册进 esp_console,BLE 侧在同一个
// 表上自己派发。刻意不用 esp_console_run():它的内置 help 命令直接 printf 到
// stdout,BLE 侧一个字也看不到。
//
// 密码处理:命令只在终端里回显一次(本地配置不可避免),但**绝不写进日志、也不回读**。
// love_net_status_t 里唯一带密码的是 ap_pass,那是热点密码(每台随机生成一次、存 NVS,
// 本来就印在设备屏幕上),用户配置的那个密码只有写入路径、没有读出接口。
#include "love_console.h"

#include "love_app.h"
#include "love_ble.h"
#include "love_console_line.h"
#include "love_event_order.h"
#include "love_httpd.h"
#include "love_net.h"
#include "love_shot.h"
#include "love_store.h"
#include "love_time.h"
#include "power_sleep.h"

#include "bsp_display.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "love_console";

// 单条输出的上限。最长的一行是 wifi 状态里的"后台网页: http://10.255.234.34/"。
#define OUT_MAX 192

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static bool s_started;
static void (*s_out)(const char *text, size_t len);

static love_console_src_t src_of(void *ctx)
{
    return (love_console_src_t)(uintptr_t)ctx;
}

void love_console_set_out(void (*fn)(const char *text, size_t len))
{
    s_out = fn;
}

void love_console_out(const char *fmt, ...)
{
    char buf[OUT_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // stdout 走默认路径:设备没有 USB 主机时 usb_serial_jtag_write() 直接返回 -1,
    // 写不出去也不会卡住任何任务(见 start() 里关于非阻塞开关的说明)。
    fputs(buf, stdout);
    if (s_out) s_out(buf, strlen(buf));
}

// 蓝牙链路上的危险命令要人在设备上按一下确定:那条链路近场可连、**不需要配对**
// (见 docs/development/engineering/wifi-provisioning.md 的安全说明),所以"连上就等于
// 拿到一台无口令终端"。改 Wi-Fi、关热点、改时间这三类会改变持久状态或把主人锁在外面,
// 值得让主人按一下。USB 侧不要求 —— 插着线本身就是物理接触,而且在那里敲这些命令的
// 正是主人自己(调试/救砖)。
#define BLE_CONFIRM_TIMEOUT_MS 8000

static bool command_allowed(love_console_src_t src, const char *action)
{
    if (src != LOVE_CONSOLE_SRC_BLE) return true;
    return love_app_confirm_request(action, BLE_CONFIRM_TIMEOUT_MS);
}

static bool require_usb(love_console_src_t src, const char *what)
{
    if (src == LOVE_CONSOLE_SRC_USB) return true;
    love_console_out("%s只能从 USB 串口做(插着线说明东西在你手边)。\n", what);
    return false;
}

/* ---------- 命令实现 ---------- */

static void print_wifi_status(void)
{
    love_net_status_t net;
    love_net_get_status(&net);

    love_console_out("网络: %s", love_net_state_text(net.state));
    if (net.ip[0] != '\0') love_console_out(", IP %s", net.ip);
    love_console_out("\n");

    if (net.has_credentials) {
        love_console_out("已配置 Wi-Fi: %s\n", net.sta_ssid);
    } else if (net.ap_active) {
        love_console_out("尚未配置 Wi-Fi。热点 %s 已开启,密码见设备屏幕。\n", net.ap_ssid);
    } else {
        love_console_out("尚未配置 Wi-Fi,热点也是关闭的。\n");
    }

    // 手动关掉的热点不会自己回来(见 love_net.h):这个状态必须说清楚,否则用户会一直
    // 等它自动打开 —— 那正是这次改动之前的行为。怎么开回来统一由 ap 命令回答,别在这
    // 两处各写一遍。
    if (!net.ap_active && net.ap_manual_off) {
        love_console_out("热点为手动关闭,不会再自动打开(ap on 可打开)。\n");
    }

    const char *site = net.site_url[0] ? net.site_url
                     : (net.lan_url[0] ? net.lan_url : NULL);
    love_console_out("后台网页: %s\n", site ? site : "暂不可达");
}

// 热点是屏幕上那个开关、后台页那两个按钮之外的第三条入口:只有 USB 或蓝牙串口
// 在手边时(比如热点被手动关掉、局域网也进不去),它就是唯一能把热点开回来的地方。
static int cmd_ap(void *ctx, int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        love_net_status_t net;
        love_net_get_status(&net);
        love_console_out("热点: %s%s\n", net.ap_active ? "已打开" : "已关闭",
                         (!net.ap_active && net.ap_manual_off) ? "(手动关闭,不会再自动打开)" : "");
        return 0;
    }

    bool on;
    if (strcmp(argv[1], "on") == 0) {
        on = true;
    } else if (strcmp(argv[1], "off") == 0) {
        on = false;
    } else {
        love_console_out("用法: ap / ap on / ap off\n");
        return 1;
    }

    // 关热点会把局域网与热点两条入口一起关掉(手动关还会写进 NVS,重启也不再自动开),
    // 有必要让主人按一下。开热点不拦:密码是每台随机的,开了也进不去。
    if (!on && !command_allowed(src_of(ctx), "关闭后台热点")) {
        love_console_out("未在设备上确认,已拒绝关热点。\n");
        return 1;
    }

    esp_err_t err = on ? love_net_ap_start() : love_net_ap_stop();
    if (err != ESP_OK) {
        love_console_out("热点%s失败: %s。\n", on ? "打开" : "关闭", esp_err_to_name(err));
        return 1;
    }
    love_console_out(on ? "热点已打开。\n" : "热点已关闭,之后不会再自动打开。\n");
    return 0;
}

static int cmd_wifi(void *ctx, int argc, char **argv)
{
    const love_console_src_t src = src_of(ctx);

    if (argc == 1) {
        print_wifi_status();
        return 0;
    }

    if (strcmp(argv[1], "clear") == 0) {
        if (!command_allowed(src, "清除 Wi-Fi 凭据")) {
            love_console_out("未在设备上确认,已拒绝清除凭据。\n");
            return 1;
        }
        if (love_net_forget() != ESP_OK) {
            love_console_out("清除凭据失败。\n");
            return 1;
        }
        love_console_out("凭据已清除,热点已打开,可以重新配网。\n");
        return 0;
    }

    // wifi open <名称> 连接开放网络;wifi <名称> <密码> 连接加密网络。
    const bool open = (strcmp(argv[1], "open") == 0);
    const int ssid_index = open ? 2 : 1;
    if (argc <= ssid_index || (!open && argc <= ssid_index + 1)) {
        love_console_out("用法:\n"
                         "  wifi                 查看当前状态\n"
                         "  wifi <名称> <密码>   保存并连接\n"
                         "  wifi open <名称>     连接开放网络\n"
                         "  wifi clear           清除已保存的凭据\n");
        return 1;
    }

    const char *ssid = argv[ssid_index];
    const char *pass = open ? "" : argv[ssid_index + 1];

    // 写入凭据会把设备牵到另一个网络上(可能是攻击者的 AP),蓝牙链路上要求机身确认。
    // 确认页只放得下一行说明,所以这里只报"要做什么",不倒用户名。
    if (!command_allowed(src, open ? "连接一个开放 Wi-Fi" : "写入新的 Wi-Fi 凭据")) {
        love_console_out("未在设备上确认,已拒绝写入凭据。\n");
        return 1;
    }

    esp_err_t err = love_net_set_credentials(ssid, pass);
    if (err != ESP_OK) {
        love_console_out("保存 \"%s\" 失败: %s。\n", ssid, esp_err_to_name(err));
        return 1;
    }
    love_console_out("已保存 \"%s\"(%s),正在连接;稍后用 wifi 查看结果。\n",
                     ssid, open ? "开放网络" : "已加密");
    return 0;
}

static int cmd_ble(void *ctx, int argc, char **argv)
{
    const love_console_src_t src = src_of(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        love_console_out("蓝牙: %s\n", love_ble_state_text());
        return 0;
    }

    bool on;
    if (strcmp(argv[1], "on") == 0) {
        on = true;
    } else if (strcmp(argv[1], "off") == 0) {
        on = false;
    } else {
        love_console_out("用法: ble / ble on / ble off\n");
        return 1;
    }

    if (!on && src == LOVE_CONSOLE_SRC_BLE) {
        // 关栈会把这条连接一起断掉,回复根本发不出去。交给 love_ble 的工作任务:
        // 它先把这一行通知发完,再隔一拍执行关闭。
        love_console_out("蓝牙即将关闭,连接会断开。\n");
        love_ble_request_stop();
        return 0;
    }

    esp_err_t err = love_app_set_ble(on);
    if (err != ESP_OK) {
        love_console_out("蓝牙%s失败: %s。\n", on ? "打开" : "关闭", esp_err_to_name(err));
        return 1;
    }
    love_console_out("蓝牙已%s。\n", on ? "打开" : "关闭");
    return 0;
}

static int cmd_time(void *ctx, int argc, char **argv)
{
    const love_console_src_t src = src_of(ctx);

    if (argc == 1) {
        love_time_state_t state;
        love_time_get(&state);
        if (!state.holds) {
            love_console_out("时间未同步。\n");
            return 0;
        }
        char described[48] = { 0 };
        love_time_describe(&state, described, sizeof(described));
        love_console_out("%s\n", described);
        return 0;
    }

    char *end = NULL;
    const unsigned long long value = strtoull(argv[1], &end, 10);
    // 越界在 love_time_set() 里是**异步**被忽略的,只看它的返回值会给出
    // "已对时"的假象,所以这里按同一套上下界先挡一道。
    if (end == argv[1] || *end != '\0' || value == 0) {
        love_console_out("要写成 Unix 秒(十进制整数),例如 time 1750000000。\n");
        return 1;
    }
    if (value < LOVE_TIME_EPOCH_MIN || value > LOVE_TIME_EPOCH_MAX) {
        love_console_out("这个时间戳不在 2020–2100 之间,拒绝写入。\n");
        return 1;
    }
    // 时间会写进 NVS,而且直接决定主屏"在一起多少天"显示什么,蓝牙链路上要机身确认。
    if (!command_allowed(src, "把设备时间改成这个值")) {
        love_console_out("未在设备上确认,已拒绝改时间。\n");
        return 1;
    }
    if (love_time_set((uint64_t)value, LOVE_TIME_SRC_CONSOLE) != ESP_OK) {
        love_console_out("对时失败。\n");
        return 1;
    }
    love_console_out("已按串口对时。\n");
    return 0;
}

// 几个关键任务的剩余栈(字节)。v4 起一个 love_config_t 就是 1454 字节,而它是整份
// 落在调用它的任务栈上的 —— 这几个数就是"还能不能再加事件条数或字段"的判断依据,
// 也是本仓库反复用到的那类实测数据。取不到的任务直接跳过(比如蓝牙没开时没有
// nimble_host 任务,蓝牙关掉后 love_ble_con 也会自己退出)。
//
// 按键那一项(任务名 input,见 main.c)才是"机身按键 → 整屏重绘"的真实执行者:
// BSP 的按键回调跑在 esp_timer 上,但它只做一件事 —— 把事件丢进队列立刻返回,
// 真正的 love_app_key/render 在 input 任务(4096)里跑。所以要看界面深度的余量,
// 看 input;esp_timer 只反映驱动回调那一小段。顺带记一笔:esp_timer 的栈不是
// CONFIG_ESP_TIMER_TASK_STACK_SIZE(3584),IDF 还给非 nano 格式化加了 512,
// 实际是 4096 —— 这个值只能从水位反推,别按 3584 算余量。
static void print_stack_headroom(void)
{
    static const struct {
        const char *task;
        const char *label;
    } TASKS[] = {
        { "console_repl", "控制台" },
        { "httpd",        "网页" },
        { "taskLVGL",     "界面" },
        { "input",        "按键" },
        { "nimble_host",  "蓝牙" },
        { "love_ble_con", "蓝牙台" },
        { "esp_timer",    "定时" },
    };

    char line[OUT_MAX];
    int used = snprintf(line, sizeof(line), "栈余");
    for (size_t i = 0; i < ARRAY_SIZE(TASKS); i++) {
        TaskHandle_t handle = xTaskGetHandle(TASKS[i].task);
        if (!handle || used >= (int)sizeof(line)) continue;
        const size_t free_bytes = (size_t)uxTaskGetStackHighWaterMark(handle) *
                                  sizeof(StackType_t);
        used += snprintf(line + used, sizeof(line) - (size_t)used, " %s %u",
                         TASKS[i].label, (unsigned)free_bytes);
    }
    love_console_out("%s\n", line);
}

// 两组显示序。设备屏幕上看不出"为什么是这个顺序",改完分类或网页上的顺序后
// 敲 status 就能核对分组与组内排序 —— 轮播上"哪几页、每页哪几条"就建在这两个顺序上,
// 顺序错了整屏都是错的。
static void print_event_order(void)
{
    // 一份 love_config_t 有 1454 字节,而控制台任务只有 4KB 栈(esp_console 自己还压着
    // 一层),放栈上实测只剩三百多字节余量。这份数据只是看一眼就丢,直接走堆。
    love_config_t *cfg = malloc(sizeof(*cfg));
    if (!cfg) return;
    love_store_load_config(cfg);
    if (cfg->event_count == 0) {
        free(cfg);
        return;
    }

    love_time_state_t state;
    love_time_get(&state);
    love_date_t today = { 0, 0, 0 };
    const bool holds = state.holds;
    if (holds) today = love_date_from_epoch(state.epoch_seconds, LOVE_TZ_OFFSET_SECONDS);

    uint8_t order[LOVE_EVENT_MAX];
    char line[OUT_MAX];

    const uint8_t filters[2] = { LOVE_EVENT_VIEW_LIST, LOVE_EVENT_VIEW_PAGE };
    const char *labels[2] = { "列表序", "单页序" };
    for (int f = 0; f < 2; f++) {
        const size_t count = love_event_order_build(cfg->events, cfg->event_count,
                                                    filters[f], today, holds,
                                                    order, sizeof(order));
        if (count == 0) continue;

        // 24 条事件一行放不下(单条输出有 192 字节上限,也是蓝牙通知的分片上限),
        // 所以写满一行就发一行再接着写,别让后面的事件被静默截掉。
        int used = snprintf(line, sizeof(line), "%s", labels[f]);
        for (size_t i = 0; i < count; i++) {
            const love_event_t *event = &cfg->events[order[i]];
            const int written = snprintf(line + used, sizeof(line) - (size_t)used,
                                         " %s[%s]", event->name,
                                         event->category[0] ? event->category : "未分类");
            if (written > 0 && used + written < (int)sizeof(line) - 16) {
                used += written;
                continue;
            }
            love_console_out("%s\n", line);
            used = snprintf(line, sizeof(line), "    ");
        }
        love_console_out("%s\n", line);
    }
    free(cfg);
}

// LVGL 自己那块内存池(与系统堆分开,见 sdkconfig.defaults 的 LV_MEM_SIZE_KILOBYTES)。
// 池子配小了不会报错,只会表现为"某些控件没画出来"——对象创建失败是静默的,
// 所以这里把峰值用量摆出来:sdkconfig.defaults 里"18KB 够用,若出现控件创建失败
// 再往上调"这句判断,靠的就是这几个数,而不是靠肉眼看屏幕猜。
static void print_lvgl_pool(void)
{
    if (!bsp_lvgl_lock(200)) return;
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    bsp_lvgl_unlock();

    love_console_out("界面池 共 %u, 峰值占用 %u, 剩余 %u(最大可分配 %u)\n",
                     (unsigned)mon.total_size, (unsigned)mon.max_used,
                     (unsigned)mon.free_size, (unsigned)mon.free_biggest_size);
}

static int cmd_status(void *ctx, int argc, char **argv)
{
    (void)ctx;
    (void)argc;
    (void)argv;

    love_time_state_t time_state;
    love_time_get(&time_state);
    if (time_state.holds) {
        char described[48] = { 0 };
        love_time_describe(&time_state, described, sizeof(described));
        love_console_out("时间  %s\n", described);
    } else {
        love_console_out("时间  %s\n",
                         time_state.wifi_pending ? "等待网络对时" : "未同步");
    }

    love_net_status_t net;
    love_net_get_status(&net);
    love_console_out("网络  %s%s%s\n", love_net_state_text(net.state),
                     net.ip[0] ? ", IP " : "", net.ip);
    love_console_out("蓝牙  %s\n", love_ble_state_text());
    // 熄屏状态只体现在背光上,从截图看不出来(截图读的是帧缓冲)。摆出来才验得了
    // "熄屏后按任意键只亮屏、不执行动作"这条行为。
    love_console_out("屏幕  %s\n", love_app_screen_off() ? "已熄屏" : "亮");
    // 停在哪一页同样是截图看不全的东西:截图每次都要重开串口(复位芯片),而翻页是
    // 内存里的状态。要验"上/下 一下翻一页、到两头回主页",就敲 key down / status 交替看。
    char view[120];
    love_app_view_text(view, sizeof(view));
    love_console_out("界面  %s\n", view);
    // "它为什么不睡"必须能被读出来,而不是靠猜:自动深睡要同时满足按键、网页、蓝牙
    // 三道闸门,而且熄屏档位设成"常亮"时按设计根本不睡。这里把四个数一起摆出来
    // (最容易被忽略的是手机后台页还在每 10 秒轮询,以及档位是常亮)。
    const uint32_t web_idle = love_httpd_client_idle_seconds();
    const uint32_t blank_off = love_app_blank_off_seconds();
    char web_text[20];
    char blank_text[16];
    if (web_idle == UINT32_MAX) snprintf(web_text, sizeof(web_text), "从未请求");
    else snprintf(web_text, sizeof(web_text), "%u 秒", (unsigned)web_idle);
    if (blank_off == 0) snprintf(blank_text, sizeof(blank_text), "常亮");
    else snprintf(blank_text, sizeof(blank_text), "%u 秒", (unsigned)blank_off);
    love_console_out("空闲  按键 %u 秒(深睡阈 %u) 熄屏档 %s 网页 %s 蓝牙 %s\n",
                     (unsigned)love_app_idle_seconds(),
                     (unsigned)love_app_deep_sleep_after_seconds(),
                     blank_text, web_text,
                     love_ble_connected() ? "已连接(不睡)" : "无连接");
    // 本次是上电、定时唤醒还是按键唤醒。深睡时 USB 断电,启动最早那几行日志主机
    // 常常接不住,所以这个原因只能问接口 —— 验"按键唤醒深睡"就靠它。
    love_console_out("唤醒  %s\n", power_sleep_wake_text());
    // 本次若是"上电/复位"(比如刚被主机开串口复位过),把上一次深睡的真实原因也报出来,
    // 否则那条信息就永远丢了(见 power_sleep_last_deep_wake_text)。
    if (strcmp(power_sleep_wake_text(), "上电/复位") == 0 &&
        power_sleep_last_deep_wake_text() != NULL) {
        love_console_out("深睡  上次由 %s 唤醒\n", power_sleep_last_deep_wake_text());
    }
    if (love_app_debug_mode()) {
        love_console_out("调试  开(不熄屏、不自动深睡;debug off 关掉)\n");
    }

    // 这两个数比"剩余堆"更能预测网页能不能传大文件:Wi-Fi 驱动发一帧要一块
    // 约 1600 字节的连续内存,连续块不够时页面就传不动。
    love_console_out("内存  剩余 %u, 最大连续块 %u\n",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    print_stack_headroom();
    print_lvgl_pool();
    print_event_order();
    return 0;
}

// 调试用:把一次按键注入应用,等价于真的按一下。
// 存在的理由:截图协议只能看到"当前那一屏",没有这个入口就走不到别的屏去核对 ——
// 列表、卡片、设置页的排版都验不了。参数上/下/确定/长按(长按=确定键长按)。
static int cmd_key(void *ctx, int argc, char **argv)
{
    // **仅 USB**:它能驱动整个界面(包括开关蓝牙/热点、触发深睡眠),蓝牙链路上等于
    // 把"遥控器"交给任何连上来的近场设备。它是调试工具,插着线时用就够了。
    if (!require_usb(src_of(ctx), "按键注入")) return 1;

    bsp_btn_t btn;
    bsp_btn_ev_t ev = BSP_BTN_CLICK;
    if (argc < 2) {
        love_console_out("用法: key up|down|ok|long\n");
        return 1;
    }
    if (strcmp(argv[1], "up") == 0) {
        btn = BSP_BTN_UP;
    } else if (strcmp(argv[1], "down") == 0) {
        btn = BSP_BTN_DOWN;
    } else if (strcmp(argv[1], "ok") == 0) {
        btn = BSP_BTN_OK;
    } else if (strcmp(argv[1], "long") == 0) {
        btn = BSP_BTN_OK;
        ev = BSP_BTN_LONG;
    } else {
        love_console_out("用法: key up|down|ok|long\n");
        return 1;
    }

    love_app_key(btn, ev);
    return 0;
}

// 休眠调试。深睡眠只能从机身状态页那两个操作项触发,而那条路要先走进设置页、
// 又要按对行 —— 没有屏幕或按键不可靠时根本验不了"睡下去之后是谁把它叫醒的"。
// 这条命令把状态页那两个操作项搬到串口上(键注入 key 命令是同一个思路)。
static int cmd_sleep(void *ctx, int argc, char **argv)
{
    // **仅 USB**:深睡眠会切断蓝牙与 USB 两条链路,是纯粹的拒绝服务手段。
    if (!require_usb(src_of(ctx), "休眠调试")) return 1;

    if (argc < 2) {
        love_console_out("用法: sleep light / sleep deep [秒]"
                         "(深睡默认 %d 秒定时唤醒,秒数给 0 = 睡到有人按键)\n",
                         (int)POWER_SLEEP_DEEP_SECONDS);
        return 1;
    }

    if (strcmp(argv[1], "light") == 0) {
        if (power_sleep_light() != ESP_OK) {
            love_console_out("浅睡眠请求失败。\n");
            return 1;
        }
        love_console_out("开始浅睡眠 %d 秒。\n", (int)POWER_SLEEP_LIGHT_SECONDS);
        return 0;
    }

    if (strcmp(argv[1], "deep") == 0) {
        unsigned long seconds = POWER_SLEEP_DEEP_SECONDS;
        if (argc >= 3) {
            char *end = NULL;
            seconds = strtoul(argv[2], &end, 10);
            if (end == argv[2] || *end != '\0' || seconds > 86400) {
                love_console_out("秒数要写成 0~86400 的十进制整数(0 = 睡到有人按键)。\n");
                return 1;
            }
        }
        // 成功就不会返回(设备直接睡下去,唤醒是一次新的开机),所以下面这句只在失败时打得出来。
        // **秒数给 0** 是 idle 路径那条形态:只靠机身按键唤醒,可以用来验证"按键能不能叫醒它"。
        if (love_app_sleep_deep((uint32_t)seconds) != ESP_OK) {
            love_console_out("深睡眠请求失败(没睡成,详情见日志)。\n");
            return 1;
        }
        love_console_out("开始深睡眠。\n");
        return 0;
    }

    love_console_out("用法: sleep light / sleep deep [秒]\n");
    return 1;
}

// 调试模式:开着时不熄屏、不自动深睡(存 NVS,重启也算数)。
// **只能从 USB 打开**:蓝牙链路上把它打开等于让设备整晚亮屏不睡(耗电),
// 而调试本来就要插线;关掉则允许从任何链路 —— 关掉只是恢复默认行为,没有危害。
static int cmd_debug(void *ctx, int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        love_console_out("调试模式: %s(不熄屏、不自动深睡)\n",
                         love_app_debug_mode() ? "开" : "关");
        return 0;
    }

    bool on;
    if (strcmp(argv[1], "on") == 0) {
        on = true;
    } else if (strcmp(argv[1], "off") == 0) {
        on = false;
    } else {
        love_console_out("用法: debug / debug status / debug on / debug off\n");
        return 1;
    }

    if (on && !require_usb(src_of(ctx), "打开调试模式")) return 1;

    love_app_set_debug(on);
    love_console_out(on ? "调试模式已打开:不熄屏、不自动深睡。\n"
                        : "调试模式已关闭:恢复正常熄屏与深睡。\n");
    return 0;
}

// 截屏的图是从 USB 串口出去的,蓝牙链路拿不到(150KB 也不适合走通知)。
static int cmd_shot(void *ctx, int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (src_of(ctx) != LOVE_CONSOLE_SRC_USB) {
        love_console_out("截图只能从 USB 串口取。\n");
        return 1;
    }
    // 成功时**什么都不打印**:二进制的图紧跟在这条命令之后,主机按声明字节数精确读取,
    // 多一行文字虽然落在字节数之外,但没必要。失败的原因由 love_shot 记进日志。
    if (!love_shot_send()) {
        love_console_out("截图未完成,详情见串口日志。\n");
        return 1;
    }
    return 0;
}

static int cmd_help(void *ctx, int argc, char **argv);

typedef struct {
    const char *name;
    const char *help;
    int (*func)(void *ctx, int argc, char **argv);
} love_command_t;

static const love_command_t COMMANDS[] = {
    { "wifi",   "配置 Wi-Fi:wifi / wifi <名称> <密码> / wifi open <名称> / wifi clear", cmd_wifi },
    { "ap",     "后台热点:ap(看状态)/ ap on / ap off(关掉后不再自动开)", cmd_ap },
    { "ble",    "蓝牙串口:ble(看状态)/ ble on / ble off", cmd_ble },
    { "time",   "对时:time 看当前时间,time <Unix 秒> 写入", cmd_time },
    { "status", "时间、网络、蓝牙、内存与事件列表序", cmd_status },
    { "shot",   "截图:把当前屏幕以 RGB565 经 USB 串口发出(见 tools/screenshot.py)", cmd_shot },
    // 发布流程按 docs/reference/y2lin/serial-screenshot-protocol.md 发的是这个字面量,
    // 所以它得是一条可用的命令名,而不是只写在文档里的约定。
    { "FAP_SCREENSHOT_V1", "同 shot", cmd_shot },
    { "debug",  "调试模式:debug status / debug on / debug off(开着不熄屏不深睡,仅 USB 可开)", cmd_debug },
    { "sleep",  "调试:sleep light / sleep deep [秒](仅 USB;秒数 0 = 睡到有人按键)", cmd_sleep },
    { "key",    "调试:注入一次按键 key up|down|ok|long(仅 USB)", cmd_key },
    { "help",   "列出所有命令", cmd_help },
};

static int cmd_help(void *ctx, int argc, char **argv)
{
    (void)ctx;
    (void)argc;
    (void)argv;

    for (size_t i = 0; i < ARRAY_SIZE(COMMANDS); i++) {
        love_console_out("%s\n    %s\n", COMMANDS[i].name, COMMANDS[i].help);
    }
    return 0;
}

/* ---------- 派发 ---------- */

int love_console_exec(char *line, love_console_src_t src)
{
    if (!line) return 0;

    char *argv[LOVE_ARGV_MAX];
    size_t argc = 0;
    const love_cmd_status_t status = love_line_split(line, argv, LOVE_ARGV_MAX, &argc);

    if (status == LOVE_CMD_EMPTY) return 0;
    if (status == LOVE_CMD_TOO_MANY_ARGS) {
        // 不截断后执行:少一个参数的 wifi 命令会把错误的东西写进 NVS。
        love_console_out("参数太多。敲 help 看用法。\n");
        return 1;
    }

    for (size_t i = 0; i < ARRAY_SIZE(COMMANDS); i++) {
        if (strcmp(argv[0], COMMANDS[i].name) == 0) {
            return COMMANDS[i].func((void *)(uintptr_t)src, (int)argc, argv);
        }
    }

    love_console_out("未知命令 \"%s\",敲 help 看支持哪些。\n", argv[0]);
    return 127;
}

/* ---------- 启动 ---------- */

esp_err_t love_console_start(void)
{
    // 幂等:love_app_start() 在深睡眠回滚后会再调一次,重复注册会多出一个
    // REPL 任务(多一份 4KB 栈)并抢同一条命令表。
    if (s_started) return ESP_OK;

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    // 历史只留在 RAM:history_save_path 保持 NULL(默认值),显式写出来是因为它是一条
    // 安全约束——敲过的命令行里有 Wi-Fi 密码,不能落到文件上。
    repl_config.history_save_path = NULL;
    // 但**不能设成 0**:linenoiseHistorySetMaxLen() 拒绝 <1,会让
    // esp_console_new_repl_usb_serial_jtag() 直接返回 ESP_FAIL(整条入口起不来,
    // 而且驱动缓冲已经分配过、白扔掉几 KB)。1 = 只保留最近一条,够用又留得最少。
    repl_config.max_history_len = 1;
    repl_config.prompt = "> ";
    // 与 BLE 侧的 love_line_t 用同一个长度上限,免得同一个命令在两个入口上限制不同。
    repl_config.max_cmdline_length = LOVE_LINE_MAX;
    // 单位是字节(IDF 的 xTaskCreate 栈深与 vanilla FreeRTOS 不同),4KB 与 love_time
    // 的工作任务一致;控制台任务本身只做行编辑与命令派发。
    repl_config.task_stack_size = 4096;
    repl_config.task_priority = 3;

    esp_console_dev_usb_serial_jtag_config_t dev_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_console_repl_t *repl = NULL;
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl);
    if (err != ESP_OK) {
        // 带上堆状况:这个失败几乎总是任务栈拿不到一块连续内存,只看错误码看不出原因。
        ESP_LOGE(TAG, "创建串口控制台失败: %s(堆余 %u,最大连续块 %u)",
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return err;
    }

    // 这里**不要**调 usb_serial_jtag_vfs_use_nonblocking()。它不但没有必要,还会把
    // 控制台打死:设备没有 USB 主机时 usb_serial_jtag_write() 本来就直接返回 -1
    // (写不出去,不阻塞),而那个开关同时把**读**也变成非阻塞 —— REPL 的阻塞读
    // 立刻返回空,于是它以最高优先级疯狂重印提示符,实测 12 秒刷了 490KB 的 "> ",
    // 顺带把 CPU 占满。所以输出直接走 stdout 的默认(阻塞)路径就是安全的。

    for (size_t i = 0; i < ARRAY_SIZE(COMMANDS); i++) {
        const esp_console_cmd_t cmd = {
            .command = COMMANDS[i].name,
            .help = COMMANDS[i].help,
            // 用带上下文的回调把"来自哪条链路"传进去,而不是读一个全局变量:
            // USB REPL 任务与 BLE 工作任务可能同时在执行命令。
            .func_w_context = COMMANDS[i].func,
            .context = (void *)(uintptr_t)LOVE_CONSOLE_SRC_USB,
        };
        err = esp_console_cmd_register(&cmd);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "注册命令 %s 失败: %s", COMMANDS[i].name, esp_err_to_name(err));
            return err;
        }
    }

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动串口控制台失败: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "USB 串口控制台已就绪:敲 help 看用法");
    return ESP_OK;
}
