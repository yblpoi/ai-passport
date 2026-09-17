// main/love_app.c —— 像素风纪念日摆件的界面与按键逻辑。
//
// 三个视图:主屏(在一起 N 天)、事件卡(逐个切换)、设置页(长按确定键)。
// 所有 LVGL 访问都在 bsp_lvgl_lock() 内;NVS 落盘、热点开关等慢操作放在锁外。
#include "love_app.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "love_ble.h"
#include "love_console.h"
#include "love_date.h"
#include "love_event_order.h"
#include "love_httpd.h"
#include "love_lunar.h"
#include "love_net.h"
#include "love_pixel_art.h"
#include "love_store.h"
#include "love_time.h"
#include "power_sleep.h"
#include "ui_pixel.h"
#include "ui_pixel_math.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 方舟像素字体(Ark Pixel)12px 的整数倍字号:12 / 24 / 36。
// 像素字体只在设计尺寸的整数倍下放大才不会出现半像素、笔画粗细不匀。
LV_FONT_DECLARE(love_font_12);
LV_FONT_DECLARE(love_font_24);
LV_FONT_DECLARE(love_font_36);

// 图标数量由素材生成器决定;这里和纯逻辑层的上限必须一致,否则后台可能存进越界图标号。
_Static_assert(LOVE_ICON_COUNT == LOVE_ICON_MAX, "图标数量与 love_date.h 的 LOVE_ICON_MAX 不一致");
_Static_assert(LOVE_ICON_PX == 40, "图标像素尺寸变化会影响界面排版");

static const char *TAG = "love_app";

#define COL_INK      0x17202A
#define COL_WHITE    0xFFFFFF
#define COL_PINK     0xF1959E
#define COL_SHADOW   0xC96F79

// 电量显示的粉白配色:文字粉、电池外框白、电量格粉。
#define COL_BAT_TEXT 0xFFE3E6
#define COL_BAT_FILL 0xF7BFC4

#define VIEW_MAIN     0
#define VIEW_SETTINGS (-1)
#define VIEW_STATUS   (-2)
// 事件列表屏(分页,一屏 4 条)。也是负数,所以**不能**再用 s_view ± 1 在视图之间跳:
// -3 加一会串到 VIEW_STATUS(-2) 去。按键一律走显式分支。
#define VIEW_LIST     (-3)
// 蓝牙危险命令的机身确认页(见 love_app_confirm_request)。同样是负数,按键走显式分支。
#define VIEW_CONFIRM  (-4)

#define SETTINGS_ROWS 9

// 列表屏一屏几行。行高 50,首行从 y=42 起,末行底部 42+3*50+48=240,
// 提示行在 y≈302,不会打架。
#define LIST_PAGE_ROWS 4
#define LIST_ROW_TOP   42
#define LIST_ROW_PITCH 50

// 本机状态页:上面九行信息(含最近一次休眠结果),下面三项操作。
#define STATUS_INFO_ROWS    9
#define STATUS_ACTION_COUNT 3
#define STATUS_ACTION_BACK  2      // 第三项是"返回",不走 action,直接切回设置页
#define STATUS_ACTION_TOP   226
#define STATUS_ACTION_PITCH 24
#define STATUS_LINE_PITCH   16

// 自动熄屏档位(秒)取自 love_store 的共享表,顺序与之严格一致;0 表示不熄屏。
static const char *const BLANK_OFF_LABELS[LOVE_BLANK_OFF_COUNT] = { "15 秒", "30 秒", "1 分钟", "3 分钟", "常亮" };


typedef enum {
    ACT_NONE = 0,
    ACT_AP_TOGGLE,
    ACT_SYNC,
    ACT_BLANK_OFF,
    ACT_STATUS,
    ACT_BLE_TOGGLE,
    ACT_SLEEP_LIGHT,
    ACT_SLEEP_DEEP,
    ACT_BACK,
} action_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_big;
static lv_obj_t *s_unit;
static lv_obj_t *s_battery;
static lv_obj_t *s_battery_fill;   // 电量格,宽度随百分比变化
static lv_obj_t *s_page;
static lv_timer_t *s_tick;
static lv_font_t s_font_12;
static lv_font_t s_font_24;

static love_config_t s_cfg;
static int s_view = VIEW_MAIN;
static int s_sel;
// 两组显示序(v4 起每个事件自己挑展示方式):列表屏只收"列表"事件,单页卡只认
// "单页"事件。顺序由 love_event_order 决定,随"今天"变化(组内按远近排),
// 所以每次进列表/重绘前都重算一次 —— 最多二十几条,代价可忽略。
static uint8_t s_list_order[LOVE_EVENT_MAX];
static int s_list_count;
static uint8_t s_page_order[LOVE_EVENT_MAX];
static int s_page_count;
static bool s_services_ready;
static bool s_store_ready;
static int s_last_day = -1;
static char s_note[48];
static int s_note_ttl;
// 熄屏:最后一次按下的时刻(esp_timer 微秒)与当前是否已熄屏。
static int64_t s_last_input_us;
static bool s_screen_off;

// 蓝牙链路上的危险命令要人在机身上按一下确定才执行(见 love_app_confirm_request):
// 蓝牙是"近场但无需配对"的链路,任何人在旁边连上就能敲命令。结论用信号量交给
// 正在等待的控制台任务;超时由 tick 判定为拒绝。
static SemaphoreHandle_t s_confirm_done;
static volatile int s_confirm_verdict;   // 0 = 未决,1 = 允许,-1 = 拒绝
static const char *s_confirm_text;
static int64_t s_confirm_deadline_us;

// 请 LVGL 任务代劳一次重绘(非 LVGL 任务只置旗子)。
// 为什么不直接调 render():重绘要走 LVGL 的控件创建与排版,实测在 4KB 栈的控制台任务上
// 只剩两三百字节余量(设置页那条路测到过 324);而确认页是**攻击者能触发**的路径
// (BLE 上敲 wifi / ap off / time),不能让一次远程敲击把控制台任务顶穿。tick 每秒都跑,
// 最坏晚一秒出画面,而窗口有 8 秒。
static volatile bool s_render_request;

static void render(void);
static void set_note(const char *text);


/* ---------- 小工具 ---------- */

// 主字号(24px)与辅助字号(12px)。像素字体只有这两种正文尺寸,别随手加第三种:
// 非整数倍放大会让笔画粗细不匀,失去点阵观感。
static lv_obj_t *cjk_label(lv_obj_t *parent, const char *text, uint32_t color)
{
    return ui_pixel_label(parent, text, &s_font_24, color);
}

static lv_obj_t *cjk_small(lv_obj_t *parent, const char *text, uint32_t color)
{
    return ui_pixel_label(parent, text, &s_font_12, color);
}


static bool time_today(love_date_t *today)
{
    love_time_state_t state;
    love_time_get(&state);
    if (!state.holds) return false;
    *today = love_date_from_epoch(state.epoch_seconds, LOVE_TZ_OFFSET_SECONDS);
    return true;
}

/* ---------- 断电前的天数快照 ---------- */

// 设备没有 RTC。重启后到对上时之间,用断电前存下的天数顶上,别让用户开机就看见 "--"。
// 返回是否有可显示的值;*stale 为 true 表示这个值来自快照而非实时时间,界面要标注。
static bool main_days(int32_t *days, bool *stale)
{
    love_date_t today;
    if (time_today(&today)) {
        *days = love_days_together(s_cfg.start, today);
        *stale = false;
        return true;
    }

    int32_t cached = 0;
    uint64_t epoch = 0;
    if (love_store_load_days_cache(&cached, &epoch)) {
        *days = cached;
        *stale = true;
        return true;
    }
    *days = 0;
    *stale = true;
    return false;
}

// 把当前算出的天数连同当时的 UTC 秒落盘。只在天数变化或对时后调用,避免频繁擦写 NVS。
static void save_days_cache(int32_t days)
{
    love_time_state_t state;
    love_time_get(&state);
    if (!state.holds) return;
    if (love_store_save_days_cache(days, state.epoch_seconds) != ESP_OK) {
        ESP_LOGW(TAG, "天数快照保存失败");
    }
}


/* ---------- 右上角电量(粉白配色) ---------- */

#define BAT_SHELL_W 22
#define BAT_SHELL_H 11
#define BAT_BORDER  2
#define BAT_FILL_W  (BAT_SHELL_W - BAT_BORDER * 2)
#define BAT_FILL_H  (BAT_SHELL_H - BAT_BORDER * 2)

// 百分比 -> 电量格宽度。-1(读不到)时按 0 处理,文字另行显示 "--"。
static int battery_fill_width(int soc)
{
    if (soc < 0) soc = 0;
    if (soc > 100) soc = 100;
    return (BAT_FILL_W * soc + 50) / 100;
}

// 白色外框 + 粉色电量格的像素电池,右侧跟同色系的百分比文字。
static void build_battery(lv_obj_t *parent)
{
    lv_obj_t *row = ui_pixel_plain(parent);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(row, LV_ALIGN_TOP_RIGHT, -6, 6);

    lv_obj_t *shell = lv_obj_create(row);
    lv_obj_remove_flag(shell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(shell, BAT_SHELL_W, BAT_SHELL_H);
    lv_obj_set_style_radius(shell, 2, 0);
    lv_obj_set_style_bg_opa(shell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shell, BAT_BORDER, 0);
    lv_obj_set_style_border_color(shell, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_pad_all(shell, 0, 0);

    s_battery_fill = ui_pixel_plain(shell);
    lv_obj_set_style_radius(s_battery_fill, 0, 0);
    lv_obj_set_style_bg_color(s_battery_fill, lv_color_hex(COL_BAT_FILL), 0);
    lv_obj_set_style_bg_opa(s_battery_fill, LV_OPA_COVER, 0);
    lv_obj_set_size(s_battery_fill, 0, BAT_FILL_H);
    lv_obj_align(s_battery_fill, LV_ALIGN_LEFT_MID, 0, 0);

    s_battery = ui_pixel_label(row, "-- %", &s_font_12, COL_BAT_TEXT);

    // 建好就直接填一次,免得等下一秒 tick 才出数。
    int soc = bsp_battery_soc();
    if (soc < 0) lv_label_set_text(s_battery, "-- %");
    else lv_label_set_text_fmt(s_battery, "%d%%", soc);
    lv_obj_set_width(s_battery_fill, battery_fill_width(soc));
}


// 主屏/事件卡共用的“大数字”排版。36px 像素数字每字 18px 宽，10 位数也放得下。
static void add_big_number(lv_obj_t *parent, int32_t value, bool holds, int y)
{
    s_big = ui_pixel_label(parent, "", &love_font_36, COL_WHITE);
    lv_label_set_text_fmt(s_big, holds ? "%d" : "--", (int)value);
    lv_obj_align(s_big, LV_ALIGN_TOP_MID, 0, y);
}

/* ---------- 图标解析:内置素材 / 自定义头像 ---------- */

// 自定义头像在 NVS 里是 40x40 4bpp(每字节两个像素、高半字节在前),索引指向
// love_pixel_palette。渲染前解成 ARGB8888。
// arena 按"一屏里最多同时出现几个自定义头像"分配。三个视图互斥:主屏是最多的,
// 只有两个人像;事件卡只有一个图标,本机状态页没有图标。所以 2 个槽位就够,
// 同槽位在一次渲染内复用同一块缓冲,不重复解码、不动态分配。
// (原先是 3,按"两个人 + 一张事件卡"算的,但这两者从不会同屏。)
#define AVATAR_DECODE_MAX 2
static uint8_t s_avatar_px[AVATAR_DECODE_MAX][LOVE_ICON_PX * LOVE_ICON_PX * 4];
static lv_image_dsc_t s_avatar_dsc[AVATAR_DECODE_MAX];
static int s_avatar_slot[AVATAR_DECODE_MAX];
static int s_avatar_used;

// 头像圆角:半径必须与 assets/images/love_pixel_art_gen.py 的 ICON_CORNER_RADIUS 一致。
// 内置图标是在生成器里把角落像素改成透明的(调色板索引 0 即透明),自定义头像的
// 调色板没有透明项,只能在这里解码时把角落的 alpha 置 0 —— 两边视觉上才是同一套圆角。
// 存的 4bpp 数据不动,所以上传、存储、网页缩略图都不受影响。
//
// 判定式在 ui_pixel_math.c 里(纯逻辑,有主机测试保证四角对称);早先本地写过一版
// "负数当哨兵"的判断,结果只有右下角生效,别再内联一份。
#define AVATAR_CORNER_RADIUS 4

static const lv_image_dsc_t *resolve_icon(uint8_t icon)
{
    if (icon < LOVE_ICON_MAX) return love_pixel_icon(icon);

    const int slot = (int)icon - LOVE_ICON_MAX;
    if (slot >= LOVE_AVATAR_MAX) return love_pixel_icon(0);

    for (int i = 0; i < s_avatar_used; i++) {
        if (s_avatar_slot[i] == slot) return &s_avatar_dsc[i];
    }
    if (s_avatar_used >= AVATAR_DECODE_MAX) return love_pixel_icon(0);

    static uint8_t packed[LOVE_AVATAR_BYTES];
    if (love_store_load_avatar((uint8_t)slot, packed, sizeof(packed)) != LOVE_AVATAR_BYTES) {
        return love_pixel_icon(0);   // 该槽位还没上传过,退回内置图标而不是留空
    }

    const int idx = s_avatar_used++;
    uint8_t *dst = s_avatar_px[idx];
    for (int y = 0; y < LOVE_ICON_PX; y++) {
        for (int x = 0; x < LOVE_ICON_PX; x++) {
            const size_t p = (size_t)y * LOVE_ICON_PX + (size_t)x;
            const uint8_t byte = packed[p / 2];
            const uint8_t code = (p % 2 == 0) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F);
            const uint32_t rgb = love_pixel_palette[code];
            dst[p * 4 + 0] = (uint8_t)(rgb & 0xFF);          // B
            dst[p * 4 + 1] = (uint8_t)((rgb >> 8) & 0xFF);   // G
            dst[p * 4 + 2] = (uint8_t)((rgb >> 16) & 0xFF);  // R
            dst[p * 4 + 3] = ui_pixel_corner_cut(x, y, LOVE_ICON_PX, LOVE_ICON_PX,
                                                 AVATAR_CORNER_RADIUS) ? 0x00 : 0xFF;   // A
        }
    }
    const size_t pixels = (size_t)LOVE_ICON_PX * LOVE_ICON_PX;

    lv_image_dsc_t *dsc = &s_avatar_dsc[idx];
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
    dsc->header.w = LOVE_ICON_PX;
    dsc->header.h = LOVE_ICON_PX;
    dsc->header.stride = LOVE_ICON_PX * 4;
    dsc->data_size = (uint32_t)(pixels * 4);
    dsc->data = dst;
    s_avatar_slot[idx] = slot;
    return dsc;
}

/* ---------- 视图:主屏 ---------- */

// 日期行:编辑态用【】标出当前字段,小屏上也能看清焦点在哪。
static void format_date_line(char *out, size_t size, const char *prefix, love_date_t date)
{
    // 非法日期与 love_date_format 的空串输出保持一致,只留前缀。
    if (!love_date_valid(date)) {
        snprintf(out, size, "%s %s", prefix, "");
        return;
    }
    snprintf(out, size, "%s %04d-%02d-%02d", prefix, (int)date.year, (int)date.month,
             (int)date.day);
}

// 事件卡的目标日期行。农历事件要同时给出"农历几月几号"和折算出的公历日期 ——
// 只给农历用户看不出是哪天,只给公历用户对不上节日。放在 render() 之前。
static void format_event_line(char *out, size_t size, const love_event_t *event,
                              const love_countdown_t *countdown, bool unresolved)
{
    if (event->kind != LOVE_EVENT_LUNAR) {
        format_date_line(out, size, "目标日", countdown->target);
        return;
    }

    char lunar_text[24];
    love_lunar_format(event->date.month, event->date.day, lunar_text, sizeof(lunar_text));

    if (unresolved) {
        snprintf(out, size, "农历%s", lunar_text);
        return;
    }

    char solar[16];
    love_date_format(countdown->target, solar, sizeof(solar));
    snprintf(out, size, "农历%s %s", lunar_text, solar);
}

/* ---------- 自动熄屏 ---------- */
// 放在设置页之前:build_settings() 要用 blank_off_index() 显示当前档位。

#define BL_ON_LEVEL 80

static uint16_t blank_off_seconds(void)
{
    return s_cfg.blank_off_seconds;
}

// 把当前配置换算成档位下标(用于设置页显示);找不到时落到默认档。
static int blank_off_index(void)
{
    for (int i = 0; i < LOVE_BLANK_OFF_COUNT; i++) {
        if (LOVE_BLANK_OFF_SECONDS[i] == s_cfg.blank_off_seconds) return i;
    }
    return 1;
}

static void blank_off_index_step(int delta)
{
    int i = (blank_off_index() + delta + LOVE_BLANK_OFF_COUNT) % LOVE_BLANK_OFF_COUNT;
    s_cfg.blank_off_seconds = LOVE_BLANK_OFF_SECONDS[i];
}

// 记住"最近一次操作",熄屏计时从这里算起。
static void note_input(void)
{
    s_last_input_us = esp_timer_get_time();
}

static void screen_wake(void)
{
    if (!s_screen_off) return;
    s_screen_off = false;
    bsp_display_backlight(BL_ON_LEVEL);
}

static void screen_off(void)
{
    if (s_screen_off) return;
    s_screen_off = true;
    bsp_display_backlight(0);
}

// 每 tick 检查一次是否该熄屏。0 = 常亮,不熄。
static void blank_off_poll(void)
{
    uint16_t limit = blank_off_seconds();
    if (limit == 0) {
        screen_wake();
        return;
    }
    if (s_screen_off) return;
    int64_t idle_us = esp_timer_get_time() - s_last_input_us;
    if (idle_us >= (int64_t)limit * 1000000LL) screen_off();
}

/* ---------- 设置页 ---------- */

typedef struct {
    const char *label;
    char value[LOVE_WIFI_PASS_MAX];   // 最长的是热点密码/SSID(65 字节上限)
} setting_row_t;

// 每一行按下确定键时执行的动作,下标与 SETTINGS_ROWS 一一对应。
// 单独成表是为了让 render() 只管显示、handle_settings_key() 不必为了拿动作
// 把整页文字重建一遍。
static const action_t SETTING_ACTIONS[SETTINGS_ROWS] = {
    ACT_AP_TOGGLE,   // 后台热点
    ACT_NONE,        // 后台地址
    ACT_NONE,        // 热点密码
    ACT_NONE,        // 网络
    ACT_SYNC,        // 时间
    ACT_BLANK_OFF,   // 自动熄屏
    ACT_BLE_TOGGLE,  // 蓝牙串口(开关)
    ACT_STATUS,      // 本机状态
    ACT_BACK,        // 返回主屏
};
_Static_assert(sizeof(SETTING_ACTIONS) / sizeof(SETTING_ACTIONS[0]) == SETTINGS_ROWS,
               "设置页动作表与 SETTINGS_ROWS 行数不一致");

static int build_settings(setting_row_t *rows)
{
    love_net_status_t net;
    love_net_get_status(&net);

    love_time_state_t time_state;
    love_time_get(&time_state);

    int count = 0;

    rows[count].label = "后台热点";
    snprintf(rows[count].value, sizeof(rows[count].value), "%s", net.ap_active ? "开" : "关");
    count++;

    rows[count].label = "后台地址";
    // 热点关着时 192.168.4.1 根本不可达,要显示设备当前真正可用的入口。
    snprintf(rows[count].value, sizeof(rows[count].value), "%s",
             net.site_url[0] ? net.site_url
                             : (net.lan_url[0] ? net.lan_url : "不可达"));
    count++;

    rows[count].label = "热点密码";
    snprintf(rows[count].value, sizeof(rows[count].value), "%s", net.ap_pass);
    count++;

    rows[count].label = "网络";
    if (net.state == LOVE_NET_CONNECTED) {
        snprintf(rows[count].value, sizeof(rows[count].value), "%s", net.ip);
    } else if (net.state == LOVE_NET_CONNECTING) {
        snprintf(rows[count].value, sizeof(rows[count].value), "%s", "连接中");
    } else if (net.has_credentials) {
        snprintf(rows[count].value, sizeof(rows[count].value), "%s", "连接失败");
    } else {
        snprintf(rows[count].value, sizeof(rows[count].value), "%s", "未配置");
    }
    count++;

    rows[count].label = "时间";
    if (time_state.holds) {
        love_time_describe(&time_state, rows[count].value, sizeof(rows[count].value));
    } else {
        snprintf(rows[count].value, sizeof(rows[count].value), "%s",
                 time_state.wifi_pending ? "等待网络对时" : "未同步");
    }
    count++;

    rows[count].label = "自动熄屏";
    snprintf(rows[count].value, sizeof(rows[count].value), "%s",
             BLANK_OFF_LABELS[blank_off_index()]);
    count++;

    rows[count].label = "蓝牙串口";
    // 显示"配置状态"而不是 love_ble_state_text():打开失败时会与用户刚选的值不一致,
    // 那种失败已经由 set_note 提示了。运行时的三态在"本机状态"页看。
    snprintf(rows[count].value, sizeof(rows[count].value), "%s",
             s_cfg.ble_enabled ? "开" : "关");
    count++;

    rows[count].label = "本机状态";
    snprintf(rows[count].value, sizeof(rows[count].value), "%s", "查看");
    count++;

    rows[count].label = "返回主屏";
    snprintf(rows[count].value, sizeof(rows[count].value), "%s", "确定键");
    count++;

    return count;
}

/* ---------- 本机状态页 ---------- */

// 每行自己格式化。取不到的项写 "--",不拿假数据凑数 —— 与主屏"未同步"的处理一致。
static void status_info_lines(char lines[STATUS_INFO_ROWS][40])
{
    size_t i = 0;

    const int soc = bsp_battery_soc();
    const int mv = bsp_battery_mv();
    if (soc < 0 || mv < 0) {
        snprintf(lines[i], 40, "电量  --");
    } else {
        snprintf(lines[i], 40, "电量  %d%%  %d.%02dV", soc, mv / 1000, (mv % 1000) / 10);
    }
    i++;

    love_net_status_t net;
    love_net_get_status(&net);
    if (net.state == LOVE_NET_CONNECTED) {
        snprintf(lines[i], 40, "网络  已联网 %s", net.ip);
    } else if (net.state == LOVE_NET_CONNECTING) {
        snprintf(lines[i], 40, "网络  连接中");
    } else if (net.ap_active) {
        // 热点名由 love_net 生成,恒为 ASCII 的 "LoveCount-XXXX";限长既避免
        // 长名字撑满面板,也让编译器能证明不会截断。
        snprintf(lines[i], 40, "网络  热点 %.16s", net.ap_ssid);
    } else {
        snprintf(lines[i], 40, "网络  %s", net.has_credentials ? "未连接" : "未配置");
    }
    i++;

    love_time_state_t time_state;
    love_time_get(&time_state);
    if (time_state.holds) {
        love_date_t today = love_date_from_epoch(time_state.epoch_seconds, LOVE_TZ_OFFSET_SECONDS);
        int hour = 0, minute = 0;
        love_hms_from_epoch(time_state.epoch_seconds, LOVE_TZ_OFFSET_SECONDS, &hour, &minute, NULL);
        snprintf(lines[i], 40, "时间  %02d-%02d %02d:%02d %s",
                 (int)today.month, (int)today.day, hour, minute,
                 love_time_src_text(time_state.source));
    } else {
        snprintf(lines[i], 40, "时间  未同步");
    }
    i++;

    // 三态,别再用"广播中/未广播"两态:连上之后广播是停的,但蓝牙显然在工作,
    // 显示"未广播"会让人以为蓝牙关了。
    snprintf(lines[i], 40, "蓝牙  %s", love_ble_state_text());
    i++;

    snprintf(lines[i], 40, "内存  %u 字节", (unsigned)esp_get_free_heap_size());
    i++;

    const int64_t uptime_s = esp_timer_get_time() / 1000000;
    if (uptime_s >= 3600) {
        snprintf(lines[i], 40, "运行  %lld 时 %lld 分",
                 (long long)(uptime_s / 3600), (long long)((uptime_s % 3600) / 60));
    } else if (uptime_s >= 60) {
        snprintf(lines[i], 40, "运行  %lld 分 %lld 秒",
                 (long long)(uptime_s / 60), (long long)(uptime_s % 60));
    } else {
        snprintf(lines[i], 40, "运行  %lld 秒", (long long)uptime_s);
    }
    i++;

    power_sleep_result_t sleep;
    power_sleep_get_result(&sleep);
    if (!sleep.attempted) {
        snprintf(lines[i], 40, "休眠  --");
    } else if (sleep.ok) {
        snprintf(lines[i], 40, "休眠  浅睡 %d ms", (int)sleep.slept_ms);
    } else {
        snprintf(lines[i], 40, "休眠  失败 %s", esp_err_to_name(sleep.error));
    }
    i++;

    snprintf(lines[i], 40, "版本  %s", esp_app_get_description()->version);
    i++;

    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(lines[i], 40, "MAC   %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(lines[i], 40, "MAC   --");
    }
}

// 确认页的按键:短按确定 = 允许,长按确定 = 拒绝。其它键一概不理 —— 这一页只有两个
// 选择,别让上/下把它翻走、也别让"顺手一按"变成允许(长按要刻意按,拒绝更安全)。
// 结论用信号量交给正在等待的控制台任务;界面收尾由那个请求方做(它本来就要重绘回去)。
static void handle_confirm_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (btn != BSP_BTN_OK) return;
    if (ev == BSP_BTN_LONG) {
        s_confirm_verdict = -1;
    } else if (ev == BSP_BTN_CLICK) {
        s_confirm_verdict = 1;
    } else {
        return;
    }
    if (s_confirm_done) xSemaphoreGive(s_confirm_done);
}

// 本机状态页:七项自身状态 + 最近一次休眠结果,下面两项可执行的休眠操作。
static void build_status_page(void)
{
    lv_obj_t *title = cjk_label(s_scr, "本机状态", COL_WHITE);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 18);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 50, 216, 166, UI_PAPER);

    char lines[STATUS_INFO_ROWS][40];
    status_info_lines(lines);
    for (int i = 0; i < STATUS_INFO_ROWS; i++) {
        lv_obj_t *line = ui_pixel_label(panel, lines[i], &s_font_12, UI_INK);
        lv_obj_align(line, LV_ALIGN_TOP_LEFT, 0, i * STATUS_LINE_PITCH);
    }

    // 前两项带秒数,第三项"返回"不带。
    static const char *const NAMES[STATUS_ACTION_COUNT] = { "浅睡眠", "深睡眠", "返回" };
    const uint32_t SECONDS[STATUS_ACTION_COUNT] = {
        POWER_SLEEP_LIGHT_SECONDS, POWER_SLEEP_DEEP_SECONDS, 0,
    };
    for (int i = 0; i < STATUS_ACTION_COUNT; i++) {
        const int y = STATUS_ACTION_TOP + i * STATUS_ACTION_PITCH;
        const bool selected = i == s_sel;
        if (selected) ui_pixel_block(s_scr, 10, y - 4, 220, 22, COL_WHITE);
        char text[24];
        if (SECONDS[i]) snprintf(text, sizeof(text), "%s %u 秒", NAMES[i], (unsigned)SECONDS[i]);
        else snprintf(text, sizeof(text), "%s", NAMES[i]);
        lv_obj_t *label = cjk_small(s_scr, text, selected ? COL_INK : COL_WHITE);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 16, y);
    }
}

// 状态页按键:上/下 选操作,确定 执行,长按确定 回设置页。
static void handle_status_key(bsp_btn_t btn, bsp_btn_ev_t ev, action_t *action)
{
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        s_sel = 0;
        s_view = VIEW_SETTINGS;
        render();
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) {
        s_sel = (s_sel + STATUS_ACTION_COUNT - 1) % STATUS_ACTION_COUNT;
        render();
    } else if (btn == BSP_BTN_DOWN) {
        s_sel = (s_sel + 1) % STATUS_ACTION_COUNT;
        render();
    } else if (btn == BSP_BTN_OK) {
        if (s_sel == STATUS_ACTION_BACK) {
            s_sel = 0;
            s_view = VIEW_SETTINGS;
            render();
            return;
        }
        *action = (s_sel == 0) ? ACT_SLEEP_LIGHT : ACT_SLEEP_DEEP;
    }
}

/* ---------- 视图环与列表屏 ---------- */

// 重算两组显示序:列表屏只收"列表"事件,单页卡只认"单页"事件,各自排序。
// 分组不需要时间,组内按远近排序需要,所以 holds 为 false 时只有分组生效。
static void rebuild_order(love_date_t today, bool holds)
{
    s_list_count = (int)love_event_order_build(s_cfg.events, s_cfg.event_count,
                                               LOVE_EVENT_VIEW_LIST, today, holds,
                                               s_list_order, sizeof(s_list_order));
    s_page_count = (int)love_event_order_build(s_cfg.events, s_cfg.event_count,
                                               LOVE_EVENT_VIEW_PAGE, today, holds,
                                               s_page_order, sizeof(s_page_order));
}

// 某条事件在某一组里的位置;不在其中返回 -1。
static int position_in(const uint8_t *order, int count, int raw_index)
{
    for (int i = 0; i < count; i++) {
        if (order[i] == raw_index) return i;
    }
    return -1;
}

// 视图环上的站位:先是列表屏的每一行(0..s_list_count-1),再是每个单页事件
// (s_list_count .. s_list_count+s_page_count-1)。**主屏不在环上** —— 走到两头就回
// 主屏,也就是"循环直到切回主页"。不在环上(比如已经从列表点开了某条)返回 -1。
static int ring_position(void)
{
    if (s_view == VIEW_LIST) {
        return (s_sel >= 0 && s_sel < s_list_count) ? s_sel : -1;
    }
    if (s_view > VIEW_MAIN) {
        const int pos = position_in(s_page_order, s_page_count, s_view - 1);
        if (pos >= 0) return s_list_count + pos;
    }
    return -1;
}

// 走到环上的某个站位;越界就是"回主屏"。
static void goto_ring(int pos)
{
    const int total = s_list_count + s_page_count;
    if (pos < 0 || pos >= total) {
        s_view = VIEW_MAIN;
    } else if (pos < s_list_count) {
        s_view = VIEW_LIST;
        s_sel = pos;
    } else {
        // 单页事件的 s_view 用"原下标 + 1"编码,与主屏 0 不冲突。
        s_view = s_page_order[pos - s_list_count] + 1;
    }
    s_note[0] = '\0';
    render();
}

// 列表右列的天数文案。算不出来(没对时 / 农历超表)写 "--",与事件卡的说法一致,
// 不拿 0 冒充"就是今天"。
static void format_row_days(char *out, size_t size, const love_event_t *event,
                            love_date_t today, bool holds)
{
    if (!holds) {
        snprintf(out, size, "--");
        return;
    }
    const love_countdown_t countdown = love_event_countdown(event, today);
    if (!countdown.resolved) {
        snprintf(out, size, "--");
    } else if (countdown.upcoming) {
        snprintf(out, size, "%d 天", (int)countdown.days);
    } else {
        snprintf(out, size, "%d 天前", (int)(-countdown.days));
    }
}

// 分类标签底块的宽度:一个汉字 12px、一个 ASCII 字符 6px,再加左右各 3px 内边距。
// UTF-8 的续字节不再重复计宽。夹在 [16,100],超长交给 CLIP。
static int category_width(const char *text)
{
    int width = 6;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if ((*p & 0xC0) == 0x80) continue;   // 续字节
        width += (*p >= 0x80) ? 12 : 6;
    }
    if (width < 16) return 16;
    if (width > 100) return 100;
    return width;
}

// 从列表点开的那条单页卡:在**列表那一组**里翻,翻到头回列表(光标停在刚才那条)。
// 这是"列表里按确定看一眼"的路径,不改变这条事件的展示方式。
static void move_in_list_group(int delta)
{
    const int pos = position_in(s_list_order, s_list_count, s_view - 1);
    if (pos < 0) {
        // 这条已经不在列表组里了(后台把它改成了单页、或者删了):退回列表,
        // 别把用户留在一张既不在环上、也回不去的卡片上。
        s_view = (s_list_count > 0) ? VIEW_LIST : VIEW_MAIN;
        s_sel = 0;
        s_note[0] = '\0';
        render();
        return;
    }

    const int next = pos + delta;
    if (next < 0 || next >= s_list_count) {
        s_sel = pos;
        s_view = VIEW_LIST;
    } else {
        s_view = s_list_order[next] + 1;
    }
    s_note[0] = '\0';
    render();
}

static void open_settings(void)
{
    s_sel = 0;
    s_view = VIEW_SETTINGS;
    s_note[0] = '\0';
    render();
}

// 列表屏与单页卡上的按键:两者都在视图环上,上/下就是环上前后一格。
static void handle_ring_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    const int pos = ring_position();

    if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
        goto_ring(pos < 0 ? 0 : pos + 1);
    } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
        goto_ring(pos < 0 ? 0 : pos - 1);
    } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK && s_view == VIEW_LIST) {
        // 列表里按确定 = 看这一条的完整单页卡(倒计时、目标日都看得更清楚)。
        if (s_sel >= 0 && s_sel < s_list_count) {
            s_view = s_list_order[s_sel] + 1;
            s_note[0] = '\0';
            render();
        }
    } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        open_settings();
    }
    // 单页卡上的确定键**什么都不做**:机身上没有改日期的入口,日期一律在后台网页改。
}

static void render(void)
{
    // 这几行文字/图标都是"建好即用",不进 refresh_dynamic,所以做成局部变量。
    lv_obj_t *date_obj;
    lv_obj_t *hint_obj;
    lv_obj_t *name_obj;
    lv_obj_t *icon_obj;

    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    s_big = s_unit = s_battery = s_page = NULL;
    s_battery_fill = NULL;
    s_avatar_used = 0;   // 每次重绘重新算一遍本屏用到哪些自定义头像

    s_scr = ui_pixel_plain(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_PINK), 0);
    // 与后台网页、素材生成脚本共用同一张爱心底纹。
    lv_obj_set_style_bg_image_src(s_scr, love_pixel_bg_tile(), 0);
    lv_obj_set_style_bg_image_tiled(s_scr, true, 0);

    love_date_t today = { 0, 0, 0 };
    bool holds = time_today(&today);

    // 两组分组顺序每次重绘都算一遍:它们随"今天"变(组内按远近排),也随后台改配置变。
    rebuild_order(today, holds);

    // 事件可能在后台被删了,当前位置就失效了 —— 收敛回主屏。
    // **只判下标越界**:从列表点开一张"列表"事件的卡片是合法视图(它不是环上的站,
    // 但也不该被弹走),这里不能顺手把它也当成非法。
    if (s_view > VIEW_MAIN && s_view - 1 >= (int)s_cfg.event_count) {
        s_view = VIEW_MAIN;
    }

    // 右上角电量;取不到时显示 "--",不阻塞界面。
    // 状态页已经单列一行电量,不再重复画一个。
    if (s_view != VIEW_STATUS) build_battery(s_scr);

    if (s_view == VIEW_CONFIRM) {
        // 蓝牙危险命令的确认页。**用 24px 正文档**:这一页要"一眼看清要允许什么",
        // 12px 在正常观看距离下偏小(真机反馈)。像素字体只有 12/24/36 三档,
        // 非整数倍放大会让笔画不匀,所以放大就是换档,不能设字号。
        // 动作文案最长的"写入新的 Wi-Fi 凭据"在 200px 宽下会折成两行,面板留了余量。
        lv_obj_t *title = cjk_label(s_scr, "蓝牙请求", COL_WHITE);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

        lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 84, 216, 132, UI_PAPER);
        lv_obj_t *what = ui_pixel_label(panel, s_confirm_text ? s_confirm_text : "",
                                        &s_font_24, UI_INK);
        lv_obj_set_width(what, 200);
        lv_label_set_long_mode(what, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(what, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(what, LV_ALIGN_CENTER, 0, 0);

        hint_obj = cjk_small(s_scr, "确定 允许 · 长按确定 拒绝", COL_WHITE);
        lv_obj_align(hint_obj, LV_ALIGN_BOTTOM_MID, 0, -6);
        lv_screen_load(s_scr);
        return;
    }

    if (s_view == VIEW_SETTINGS) {
        setting_row_t rows[SETTINGS_ROWS];
        int count = build_settings(rows);
        if (s_sel >= count) s_sel = count - 1;
        if (s_sel < 0) s_sel = 0;

        lv_obj_t *title = cjk_label(s_scr, "设置", COL_WHITE);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 30);

        // 12px 字号下一行只要 22px,9 行也放得下,不必分页。
        // 起始 y 与标题(24px,占 30..54)要留出明显间距,不然标题和内容粘在一起。
        for (int i = 0; i < count; i++) {
            int y = 72 + i * 24;
            bool selected = i == s_sel;
            if (selected) ui_pixel_block(s_scr, 10, y - 4, 220, 22, COL_WHITE);
            lv_obj_t *label = cjk_small(s_scr, rows[i].label,
                                        selected ? COL_INK : COL_WHITE);
            lv_obj_align(label, LV_ALIGN_TOP_LEFT, 16, y);
            lv_obj_t *value = cjk_small(s_scr, rows[i].value,
                                        selected ? COL_INK : COL_WHITE);
            lv_obj_align(value, LV_ALIGN_TOP_RIGHT, -16, y);
        }

        // 提示行与主屏/事件卡一致用 12px;这里原先误用了 24px 的 cjk_label,
        // 既是其它屏的两倍大,整行也几乎铺满 240px 屏宽。
        hint_obj = cjk_small(s_scr, "上/下 选择 · 确定 执行", COL_WHITE);
        lv_obj_align(hint_obj, LV_ALIGN_BOTTOM_MID, 0, -6);
        lv_screen_load(s_scr);
        return;
    }

    if (s_view == VIEW_STATUS) {
        build_status_page();
        hint_obj = cjk_small(s_scr, "上/下 选择 · 确定 执行 · 长按返回", COL_WHITE);
        lv_obj_align(hint_obj, LV_ALIGN_BOTTOM_MID, 0, -6);
        lv_screen_load(s_scr);
        return;
    }

    if (s_view == VIEW_LIST) {
        // "列表"事件可能在后台网页上被删光或被改成单页。此时不能停在一张空列表上,
        // 直接落回主屏(不 return,让下面主屏那条分支接着画)。
        if (s_list_count == 0) {
            s_view = VIEW_MAIN;
        } else {
            const int pages = (s_list_count + LIST_PAGE_ROWS - 1) / LIST_PAGE_ROWS;
            const int page = s_sel / LIST_PAGE_ROWS + 1;
            s_page = ui_pixel_label(s_scr, "", &s_font_12, COL_WHITE);
            lv_label_set_text_fmt(s_page, "%d/%d", page, pages);
            lv_obj_align(s_page, LV_ALIGN_TOP_LEFT, 12, 8);

            const int first = (page - 1) * LIST_PAGE_ROWS;
            for (int row = 0; row < LIST_PAGE_ROWS; row++) {
                const int pos = first + row;
                if (pos >= s_list_count) break;

                const love_event_t *event = &s_cfg.events[s_list_order[pos]];
                const int row_top = LIST_ROW_TOP + row * LIST_ROW_PITCH;
                const bool selected = (pos == s_sel);

                // 选中块先铺,行内文字再按选中状态换色压在上面。
                if (selected) ui_pixel_block(s_scr, 10, row_top - 3, 220, 48, COL_WHITE);

                lv_obj_t *icon = lv_image_create(s_scr);
                lv_image_set_src(icon, resolve_icon(event->icon));
                lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 12, row_top + 2);

                const bool has_category = event->category[0] != '\0';
                if (has_category) {
                    const int cat_w = category_width(event->category);
                    // 选中行的底色本来就是白的,再铺一块白标签等于没画,只在未选中时铺。
                    if (!selected) ui_pixel_block(s_scr, 60, row_top + 3, cat_w, 15, COL_WHITE);
                    lv_obj_t *cat = cjk_small(s_scr, event->category, COL_INK);
                    lv_label_set_long_mode(cat, LV_LABEL_LONG_CLIP);
                    lv_obj_set_width(cat, cat_w - 6);
                    lv_obj_align(cat, LV_ALIGN_TOP_LEFT, 63, row_top + 5);
                }

                lv_obj_t *name = cjk_label(s_scr, event->name,
                                           selected ? COL_INK : COL_WHITE);
                lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
                // 宽度**不能**铺到右列去:24px 的名字会压在右对齐的天数文案上。
                // 120 让名字止于 x=180,最宽的天数文案("3 天前"约 40px)从 x≈188 起。
                lv_obj_set_width(name, 120);
                // 有分类时给分类标签让出一行,没有就整行靠中间一点。
                lv_obj_align(name, LV_ALIGN_TOP_LEFT, 60,
                             has_category ? row_top + 19 : row_top + 7);

                // 24 字节:最坏情况是 int 的 11 位数字 + " 天前"(7 字节) + 结尾。
                // 实际日期被夹在 1970..2099,最多 5 位数。
                char days[24];
                format_row_days(days, sizeof(days), event, today, holds);
                lv_obj_t *day = cjk_small(s_scr, days, selected ? COL_INK : COL_WHITE);
                lv_obj_align(day, LV_ALIGN_TOP_RIGHT, -12, row_top + 23);
            }

            hint_obj = cjk_small(s_scr, "上/下 选择 · 确定 打开 · 长按设置", COL_WHITE);
            lv_obj_align(hint_obj, LV_ALIGN_BOTTOM_MID, 0, -6);
            lv_screen_load(s_scr);
            return;
        }
    }

    if (s_view == VIEW_MAIN || s_cfg.event_count == 0) {
        // 两个人像移到标题上方并放大(40px 图标 + 24px 名字),人物先出场。
        // 这一屏的纵向坐标:上方要避开右上角电量(它占到 y=18),下方要留出提示行,
        // 整块下移到 y=56 起才既离电量够远、上下留白也均衡。改动时这几个值要一起动。
        const int CENTERS[LOVE_PERSON_MAX] = { 62, 178 };
        for (int i = 0; i < LOVE_PERSON_MAX; i++) {
            lv_obj_t *person = ui_pixel_plain(s_scr);
            lv_obj_set_pos(person, CENTERS[i] - 48, 56);
            lv_obj_set_size(person, 96, 100);
            lv_obj_set_style_bg_opa(person, LV_OPA_TRANSP, 0);

            lv_obj_t *icon = lv_image_create(person);
            lv_image_set_src(icon, resolve_icon(s_cfg.people[i].icon));
            lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);

            lv_obj_t *name = cjk_label(person, s_cfg.people[i].name, COL_WHITE);
            // 名字可能长到 8 个汉字,超出人物块宽度时裁切而不是换行,避免挤压下方排版。
            lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(name, 96);
            // 标签被拉满整块宽度,不显式居中就会左对齐,和上面居中的图标错开。
            lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 46);
        }

        lv_obj_t *title = cjk_label(s_scr, "在一起", COL_WHITE);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 142);

        int32_t days = 0;
        bool stale = false;
        bool have_days = main_days(&days, &stale);
        add_big_number(s_scr, days, have_days, 180);

        // 大数字下方是单位。实时的写「天」,用断电快照的写「天(未对时)」,
        // 完全没有可用值时写「未同步」—— 不拿旧数据冒充实时天数。
        const char *unit = !have_days ? "未同步" : (stale ? "天(未对时)" : "天");
        s_unit = cjk_small(s_scr, unit, COL_WHITE);
        lv_obj_align(s_unit, LV_ALIGN_TOP_MID, 0, 218);

        ui_pixel_block(s_scr, 60, 238, 120, 3, COL_SHADOW);

        char text[48];
        format_date_line(text, sizeof(text), "起始日", s_cfg.start);
        date_obj = cjk_small(s_scr, text, COL_WHITE);
        lv_obj_align(date_obj, LV_ALIGN_TOP_MID, 0, 250);

        // 上/下 会进环上的第一个站:有"列表"事件时是列表屏,否则直接翻单页卡。
        // 提示跟着实际情况改,别让用户按下去以为走错了屏。
        const char *main_hint = "上/下 列表 · 长按确定 设置";
        if (s_list_count == 0) {
            main_hint = (s_page_count > 0) ? "上/下 切换 · 长按确定 设置" : "长按确定 设置";
        }
        hint_obj = cjk_small(s_scr, main_hint, COL_WHITE);
        lv_obj_align(hint_obj, LV_ALIGN_BOTTOM_MID, 0, -6);
        lv_screen_load(s_scr);
        return;
    }

    // 事件卡
    int index = s_view - 1;
    if (index < 0) index = 0;
    if (index >= s_cfg.event_count) index = s_cfg.event_count - 1;
    const love_event_t *event = &s_cfg.events[index];

    // 页码按"这条属于哪一组"算:单页事件报它在单页里的位置,从列表点开的报它在列表
    // 里的位置。直接拿事件总表的下标去报(比如"3/8")对用户没有任何意义 ——
    // v4 起两种展示方式各成一组。
    const uint8_t *group = (event->view_mode == LOVE_EVENT_VIEW_PAGE) ? s_page_order
                                                                      : s_list_order;
    const int group_count = (event->view_mode == LOVE_EVENT_VIEW_PAGE) ? s_page_count
                                                                      : s_list_count;
    const int group_pos = position_in(group, group_count, index);
    s_page = ui_pixel_label(s_scr, "", &s_font_12, COL_WHITE);
    lv_label_set_text_fmt(s_page, "%d/%d", group_pos >= 0 ? group_pos + 1 : index + 1,
                          group_count > 0 ? group_count : 1);
    lv_obj_align(s_page, LV_ALIGN_TOP_LEFT, 12, 8);

    // 这一屏的纵向坐标同样整块下移过:内容只占 166px,原来从 y=40 起,
    // 底部空出近百像素。现在从 68 起,上下留白各约 68px,和主屏、设置页一致。
    icon_obj = lv_image_create(s_scr);
    lv_image_set_src(icon_obj, resolve_icon(event->icon));
    lv_obj_align(icon_obj, LV_ALIGN_TOP_MID, 0, 68);

    name_obj = cjk_label(s_scr, event->name, COL_WHITE);
    lv_label_set_long_mode(name_obj, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(name_obj, 224);
    // 同主屏人像名:标签拉满宽度后必须显式居中,否则短名字会贴在左边。
    lv_obj_set_style_text_align(name_obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name_obj, LV_ALIGN_TOP_MID, 0, 120);

    love_countdown_t countdown = { 0, true, true, event->date };
    if (holds) countdown = love_event_countdown(event, today);

    const bool lunar = (event->kind == LOVE_EVENT_LUNAR);
    // 农历表覆盖不到的年份:说清楚"算不出来",不给假数字
    const bool unresolved = holds && lunar && !countdown.resolved;

    add_big_number(s_scr, countdown.days >= 0 ? countdown.days : -countdown.days,
                   holds && !unresolved, 158);

    const char *unit;
    if (!holds) unit = "未同步";
    else if (unresolved) unit = "农历超出范围";
    else unit = countdown.upcoming ? "天后" : "天前";
    s_unit = cjk_small(s_scr, unit, COL_WHITE);
    lv_obj_align(s_unit, LV_ALIGN_TOP_MID, 0, 200);

    char date_text[48];
    format_event_line(date_text, sizeof(date_text), event, &countdown, unresolved);
    date_obj = cjk_small(s_scr, date_text, COL_WHITE);
    lv_obj_align(date_obj, LV_ALIGN_TOP_MID, 0, 222);

    // 卡片上不再能改日期(日期一律在后台网页改),所以提示只说怎么走。
    hint_obj = cjk_small(s_scr, "上/下 翻卡 · 长按确定 设置", COL_WHITE);
    lv_obj_align(hint_obj, LV_ALIGN_BOTTOM_MID, 0, -6);

    if (s_note[0]) {
        lv_obj_t *note = cjk_small(s_scr, s_note, COL_WHITE);
        lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -28);
    }
    lv_screen_load(s_scr);
}

/* ---------- 定时刷新 ---------- */

static void refresh_dynamic(void)
{
    if (s_battery) {
        int soc = bsp_battery_soc();
        if (soc < 0) lv_label_set_text(s_battery, "-- %");
        else lv_label_set_text_fmt(s_battery, "%d%%", soc);
        if (s_battery_fill) {
            lv_obj_set_width(s_battery_fill, battery_fill_width(soc));
        }
    }

    if (s_page && s_view > VIEW_MAIN && s_cfg.event_count > 0) {
        int index = s_view - 1;
        const love_event_t *event = &s_cfg.events[index];
        love_date_t today;
        if (time_today(&today) && s_big) {
            love_countdown_t countdown = love_event_countdown(event, today);
            if (!countdown.resolved) {
                // 农历年份超出数据表:数字置回占位符,别显示上次算出来的旧值
                lv_label_set_text(s_big, "--");
                if (s_unit) lv_label_set_text(s_unit, "农历超出范围");
            } else {
                lv_label_set_text_fmt(s_big, "%d",
                                      (int)(countdown.days >= 0 ? countdown.days
                                                                : -countdown.days));
                if (s_unit) lv_label_set_text(s_unit, countdown.upcoming ? "天后" : "天前");
            }
        }
    } else if (s_view == VIEW_MAIN && s_big) {
        love_date_t today;
        if (time_today(&today)) {
            lv_label_set_text_fmt(s_big, "%d", (int)love_days_together(s_cfg.start, today));
            if (s_unit) lv_label_set_text(s_unit, "天");
        }
        // 没对上时的时候保留 render() 写入的断电快照值:别每秒去读一次 NVS。
    }
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    love_net_poll();
    blank_off_poll();

    // 有人请了一次重绘(确认页出现/撤掉这类"非 LVGL 任务触发的画面变化")。
    if (s_render_request) {
        s_render_request = false;
        render();
        return;
    }

    love_date_t today;
    bool holds = time_today(&today);
    int day_key = holds ? (int)love_days_from_civil(today) : -1;
    if (day_key != s_last_day) {
        s_last_day = day_key;
        render();     // 跨天(或首次拿到时间)时整体重绘,保证天数正确
        // 拿到时间、以及每次跨天时存一次快照。只在"日子变了"时落盘,不每秒擦写 NVS。
        if (holds) save_days_cache(love_days_together(s_cfg.start, today));
        return;
    }

    if (s_note_ttl > 0 && --s_note_ttl == 0) {
        s_note[0] = '\0';
        render();
        return;
    }

    // 确认页超时 = 拒绝。等结论的那个任务(蓝牙控制台)正睡在信号量上,这里放它走。
    if (s_view == VIEW_CONFIRM && s_confirm_deadline_us != 0 &&
        esp_timer_get_time() > s_confirm_deadline_us) {
        s_confirm_deadline_us = 0;
        s_confirm_verdict = -1;
        if (s_confirm_done) xSemaphoreGive(s_confirm_done);
    }

    // 状态页的运行时长、剩余内存、休眠结果本来就是逐秒在变,整页重绘最省事;
    // 这一屏只有十几个标签,重绘代价远低于主屏(主屏有底纹与图标)。
    if (s_view == VIEW_STATUS) {
        render();
        return;
    }
    refresh_dynamic();
}

/* ---------- 外部事件 ---------- */

// 只把蓝牙的启停对齐到 want,**不动配置**。
// 必须是本文件里唯一调 love_ble_start/stop 的地方:love_ble_stop() 会等 NimBLE
// host 任务退出,所以所有调用点都得在 LVGL 锁外。
static esp_err_t ble_apply(bool on)
{
    if (on == love_ble_running()) return ESP_OK;
    return on ? love_ble_start() : love_ble_stop();
}

esp_err_t love_app_set_ble(bool on)
{
    const esp_err_t err = ble_apply(on);
    if (err != ESP_OK) return err;

    if (bsp_lvgl_lock(300)) {
        const uint8_t value = on ? 1 : 0;
        const bool changed = s_cfg.ble_enabled != value;
        s_cfg.ble_enabled = value;
        render();
        bsp_lvgl_unlock();
        // 只有真的变了才写 NVS:空闲自动关每关一次就写一遍没有必要。
        if (changed && love_store_save_config(&s_cfg) != ESP_OK) {
            ESP_LOGW(TAG, "蓝牙开关保存失败");
        }
    }
    return ESP_OK;
}

// 蓝牙要求关机:空闲超时,或者从 BLE 链路敲了 ble off。
static void on_ble_shutdown(void)
{
    const esp_err_t err = love_app_set_ble(false);
    if (bsp_lvgl_lock(300)) {
        set_note(err == ESP_OK ? "蓝牙已自动关闭" : "蓝牙关闭失败");
        render();
        bsp_lvgl_unlock();
    }
}

static void on_config_changed(void)
{
    // 后台改了配置:先重新载入,再把蓝牙启停对齐到新配置。
    // 网页上的蓝牙开关只写配置;不在这里收敛的话要等下次重启才生效(实测踩过)。
    // 这里**不能**走 love_app_set_ble():它会把整个 s_cfg 写回 NVS,而 s_cfg 还是
    // 网页保存之前的旧配置,等于把用户刚改的东西抹掉。
    // 这份配置走堆:本函数是在 httpd 任务的栈上被回调的,而它后面还要接一整屏
    // LVGL 重绘 —— 1454 字节的配置压在栈上实测会把 httpd 任务顶穿。
    love_config_t *next = malloc(sizeof(*next));
    if (!next) return;
    love_store_load_config(next);
    if (ble_apply(next->ble_enabled != 0) != ESP_OK) ESP_LOGW(TAG, "蓝牙开关未对齐");

    if (!bsp_lvgl_lock(500)) {
        free(next);
        return;
    }
    s_cfg = *next;
    free(next);
    if (s_view > (int)s_cfg.event_count) s_view = s_cfg.event_count;
    render();
    bsp_lvgl_unlock();
}

static void on_time_changed(void *ctx)
{
    (void)ctx;
    if (!bsp_lvgl_lock(500)) return;
    love_date_t today;
    bool holds = time_today(&today);
    int day_key = holds ? (int)love_days_from_civil(today) : -1;
    if (day_key != s_last_day) {
        s_last_day = day_key;
        render();
    } else {
        refresh_dynamic();
    }
    bsp_lvgl_unlock();
}

static void set_note(const char *text)
{
    snprintf(s_note, sizeof(s_note), "%s", text ? text : "");
    s_note_ttl = 4;
}

/* ---------- 按键 ---------- */

static void handle_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev, action_t *action)
{
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        s_view = VIEW_MAIN;
        render();
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP) {
        s_sel = (s_sel + SETTINGS_ROWS - 1) % SETTINGS_ROWS;
        s_note[0] = '\0';
        render();
    } else if (btn == BSP_BTN_DOWN) {
        s_sel = (s_sel + 1) % SETTINGS_ROWS;
        s_note[0] = '\0';
        render();
    } else if (btn == BSP_BTN_OK) {
        *action = SETTING_ACTIONS[s_sel];
    }
}

void love_app_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    action_t action = ACT_NONE;

    // 熄屏状态下,任意键先只负责"亮屏",不再顺带触发该键的动作 ——
    // 否则用户想看一眼天数,一按就把日期改了。
    if (s_screen_off) {
        note_input();
        if (bsp_lvgl_lock(400)) {
            screen_wake();
            bsp_lvgl_unlock();
        } else {
            bsp_display_backlight(BL_ON_LEVEL);
            s_screen_off = false;
        }
        return;
    }
    note_input();

    if (s_view != VIEW_SETTINGS) love_net_ap_touch();

    if (!bsp_lvgl_lock(400)) return;

    if (s_view == VIEW_CONFIRM) {
        handle_confirm_key(btn, ev);
    } else if (s_view == VIEW_SETTINGS) {
        handle_settings_key(btn, ev, &action);
    } else if (s_view == VIEW_STATUS) {
        handle_status_key(btn, ev, &action);
    } else if (s_view == VIEW_MAIN) {
        // 主屏。上/下 进视图环(下键从头、上键从尾)。机身上**不再提供任何改日期的
        // 入口**:日期、分类、展示方式一律在后台网页改,这里连确定键都不做动作。
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
            goto_ring(0);
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
            goto_ring(s_list_count + s_page_count - 1);
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            open_settings();
        }
    } else {
        // 列表屏或单页卡。列表里点开的那条"列表"事件虽然不在环上,但它的上/下走的是
        // 列表那一组(见 move_in_list_group),所以这里要分开判。
        const bool list_event_card = (s_view > VIEW_MAIN) &&
                                     (s_cfg.events[s_view - 1].view_mode != LOVE_EVENT_VIEW_PAGE);
        if (list_event_card) {
            if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
                move_in_list_group(1);
            } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
                move_in_list_group(-1);
            } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
                open_settings();
            }
        } else {
            handle_ring_key(btn, ev);
        }
    }
    bsp_lvgl_unlock();

    // 慢操作放在 LVGL 锁外执行。
    switch (action) {
    case ACT_AP_TOGGLE: {
        love_net_status_t net;
        love_net_get_status(&net);
        esp_err_t err = net.ap_active ? love_net_ap_stop() : love_net_ap_start();
        if (bsp_lvgl_lock(300)) {
            // 关掉之后就不再自动开(见 love_net.h),这一点必须写在提示里 ——
            // 否则用户会一直等它自己回来,而它不会了。
            set_note(err == ESP_OK ? (net.ap_active ? "热点已关闭(不再自动开)" : "热点已打开")
                                   : "热点操作失败");
            render();
            bsp_lvgl_unlock();
        }
        break;
    }
    case ACT_BLE_TOGGLE: {
        // love_ble_stop() 要等 NimBLE host 任务退出,必须在 LVGL 锁外调用;
        // 这段 switch 本来就是"慢操作",放在锁外执行。
        const bool want = !s_cfg.ble_enabled;
        const esp_err_t err = love_app_set_ble(want);
        if (bsp_lvgl_lock(300)) {
            set_note(err == ESP_OK ? (want ? "蓝牙已打开" : "蓝牙已关闭")
                                   : "蓝牙操作失败");
            render();
            bsp_lvgl_unlock();
        }
        break;
    }
    case ACT_SYNC: {
        love_net_status_t net;
        love_net_get_status(&net);
        if (net.state == LOVE_NET_CONNECTED) {
            love_time_sntp_start();
            if (bsp_lvgl_lock(300)) {
                set_note("正在网络对时…");
                render();
                bsp_lvgl_unlock();
            }
        } else if (bsp_lvgl_lock(300)) {
            set_note("请先在后台配置 Wi-Fi");
            render();
            bsp_lvgl_unlock();
        }
        break;
    }
    case ACT_BLANK_OFF:
        if (bsp_lvgl_lock(300)) {
            blank_off_index_step(1);
            // 改档位本身就是一次操作,重置计时,免得刚选完就黑屏。
            note_input();
            screen_wake();
            set_note(LOVE_BLANK_OFF_SECONDS[blank_off_index()] == 0 ? "已设为常亮" : "已更新熄屏时间");
            render();
            bsp_lvgl_unlock();
        }
        if (love_store_save_config(&s_cfg) != ESP_OK) {
            ESP_LOGW(TAG, "熄屏设置保存失败");
        }
        break;
    case ACT_STATUS:
        if (bsp_lvgl_lock(300)) {
            s_sel = 0;
            s_view = VIEW_STATUS;
            s_note[0] = '\0';
            render();
            bsp_lvgl_unlock();
        }
        break;
    case ACT_SLEEP_LIGHT:
        // 成功时界面会冻结 POWER_SLEEP_LIGHT_SECONDS 秒;唤醒后 tick 重绘状态页,
        // 「休眠」那一行会显示实际睡了多久。失败也只写进那一行,不弹提示。
        (void)power_sleep_light();
        break;
    case ACT_SLEEP_DEEP:
        // 深睡眠前必须交出 Wi-Fi/BLE/HTTP:它们持有射频与 socket,
        // 不停止就睡会让重启后的外设状态不确定。失败则把服务起回来,
        // 别把应用留在"网也没了、觉也没睡成"的状态。
        if (love_app_stop() != ESP_OK) {
            (void)love_app_start();
            break;
        }
        if (power_sleep_deep() != ESP_OK) {
            (void)love_app_start();
            if (bsp_lvgl_lock(300)) {
                s_note[0] = '\0';
                render();
                bsp_lvgl_unlock();
            }
        }
        break;
    case ACT_BACK:
        if (bsp_lvgl_lock(300)) {
            s_view = VIEW_MAIN;
            render();
            bsp_lvgl_unlock();
        }
        break;
    default:
        break;
    }
}

/* ---------- 生命周期 ---------- */


bool love_app_confirm_request(const char *action, uint32_t timeout_ms)
{
    if (!action || timeout_ms == 0) return false;
    if (s_confirm_done == NULL) {
        s_confirm_done = xSemaphoreCreateBinary();
        // 拿不到信号量就不放行:这是安全闸门,失败要往"拒绝"一侧倒。
        if (!s_confirm_done) return false;
    }
    (void)xSemaphoreTake(s_confirm_done, 0);   // 清掉上一次的残留

    if (!bsp_lvgl_lock(500)) return false;
    s_confirm_text = action;
    s_confirm_verdict = 0;
    s_confirm_deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    note_input();      // 别让确认页刚出来就被熄屏吃掉
    screen_wake();
    s_view = VIEW_CONFIRM;
    s_sel = 0;
    s_render_request = true;   // 画面交给 LVGL 任务画(见 s_render_request 的说明)
    bsp_lvgl_unlock();

    // 等结论:按键(handle_confirm_key)或 tick 超时都会 give。多等 500ms 给 tick 留余量。
    const bool got = xSemaphoreTake(s_confirm_done, pdMS_TO_TICKS(timeout_ms + 500)) == pdTRUE;
    const bool approved = got && s_confirm_verdict == 1;

    if (bsp_lvgl_lock(500)) {
        s_confirm_verdict = 0;
        s_confirm_text = NULL;
        s_confirm_deadline_us = 0;
        // 确认页撤掉后回主屏:原来的视图状态(选中行/翻到哪张卡)本来就没打算保留,
        // 回主屏是唯一不会出错的选择。
        if (s_view == VIEW_CONFIRM) {
            s_view = VIEW_MAIN;
            s_sel = 0;
        }
        s_render_request = true;
        bsp_lvgl_unlock();
    }
    note_input();      // 刚才那一下是"在设备上按的",熄屏计时从此刻算起
    return approved;
}

void love_app_enter(void)
{
    s_font_12 = love_font_12;
    s_font_24 = love_font_24;
    // 像素字体只覆盖 GB2312 一级字,缺的字与 LVGL 图标回落到 Montserrat。
    s_font_12.fallback = &lv_font_montserrat_14;
    s_font_24.fallback = &lv_font_montserrat_20;

    if (!s_store_ready) {
        if (love_store_init() != ESP_OK) {
            ESP_LOGE(TAG, "初始化存储失败,界面只能显示默认值");
        }
        s_store_ready = true;
    }
    love_store_load_config(&s_cfg);

    s_view = VIEW_MAIN;
    s_sel = 0;
    s_last_day = -1;
    s_screen_off = false;
    note_input();
    bsp_display_backlight(BL_ON_LEVEL);

    if (!s_tick) s_tick = lv_timer_create(tick, 1000, NULL);
    render();

    love_time_state_t state;
    love_time_get(&state);
    if (state.holds) {
        love_date_t today = love_date_from_epoch(state.epoch_seconds, LOVE_TZ_OFFSET_SECONDS);
        s_last_day = (int)love_days_from_civil(today);
    }
}

esp_err_t love_app_start(void)
{
    if (s_services_ready) return ESP_OK;

    if (!s_store_ready) {
        if (love_store_init() != ESP_OK) return ESP_FAIL;
        s_store_ready = true;
    }
    // 串口配网:插着 USB 时不用连热点也能配网。必须在 Wi-Fi/BLE 之前启动 ——
    // 它要一块连续的 4KB 任务栈,排在后面时堆已被 NimBLE 和 Wi-Fi 切碎,实测
    // xTaskCreatePinnedToCore 直接失败(控制台建不起来,还白扔掉驱动缓冲)。
    // 失败只少一条入口,不影响其它功能。
    if (love_console_start() != ESP_OK) {
        ESP_LOGW(TAG, "USB 串口控制台启动失败,仍可用后台网页或 BLE 配网");
    }

    love_time_init();
    (void)love_time_add_listener(on_time_changed, NULL);
    love_httpd_set_changed_cb(on_config_changed);

    if (love_net_init() != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 初始化失败,联网对时不可用");
    }
    if (love_httpd_start() != ESP_OK) {
        ESP_LOGW(TAG, "后台网页启动失败");
    }
    // 蓝牙串口(对时/配网都能走它)按配置启停,出厂默认关。关着能把 NimBLE 的
    // 任务栈与控制器缓冲还给系统堆(这个固件的堆一向紧张);开着无人连接满 5 分钟
    // 也会自动关掉并把配置写回。
    love_ble_set_shutdown_cb(on_ble_shutdown);
    if (s_cfg.ble_enabled && ble_apply(true) != ESP_OK) {
        ESP_LOGW(TAG, "蓝牙串口启动失败");
    }

    // 最大连续块和剩余总量一样重要:Wi-Fi 驱动发一帧要一块 ~1600 字节的连续内存,
    // 碎片化到拿不出来时下行会直接停摆(后台页的排障记录见 sdkconfig.defaults)。
    ESP_LOGI(TAG, "服务已启动,剩余堆 %u 字节(最大连续块 %u)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    s_services_ready = true;
    return ESP_OK;
}

esp_err_t love_app_stop(void)
{
    if (!s_services_ready) return ESP_OK;

    (void)love_ble_stop();
    love_httpd_stop();
    love_net_deinit();
    s_services_ready = false;
    return ESP_OK;
}
