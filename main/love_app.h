// main/love_app.h —— 像素风纪念日摆件的应用入口。
//
// 开机直接进入本应用,没有中间菜单。三个视图:
//   主屏  在一起 N 天 + 两个人物
//   事件卡 上/下 逐个切换事件与倒计时
//   设置页 长按确定键进入;可开关后台热点、对时、改熄屏档位、开关蓝牙串口、看本机状态
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

void love_app_enter(void);
void love_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);
esp_err_t love_app_start(void);
esp_err_t love_app_stop(void);

// 空闲检查:熄屏后长时间无人操作就进深睡眠。调用方必须是**非 LVGL 任务**
// (main.c 的 input 任务每秒调一次),理由见 love_app.c 里本函数的说明。
void love_app_idle_poll(void);

// 请求一次深睡眠(成功则设备很快睡下去;唤醒是一次重启)。
// wake_seconds = 0 表示只用机身按键唤醒(idle 路径:睡到有人按键为止);非 0 则同时设
// RTC 定时器(wake_seconds 秒后自己醒来,状态页那条操作与串口调试用)。
// 先交出 Wi-Fi/BLE/HTTP。返回值:**ESP_OK = 已经过了会失败的那一段、正在入睡**;
// 其它值 = 这次没睡成,而且服务与界面已经被恢复回去(调用方按需重绘提示即可)。
// 同时只允许一个调用者走这条路,第二个到达者立刻拿到 ESP_ERR_INVALID_STATE 并且不会
// 去动服务(**不会**把对方正在关的东西又打开)。
// 设置页那条"深睡眠"、空闲自动深睡、串口 sleep 调试命令共用它。
// **必须在非 LVGL 任务上调用**(关 BLE 会无超时等 NimBLE host 任务退出)。
esp_err_t love_app_sleep_deep(uint32_t wake_seconds);

// 屏幕是否处于熄屏状态(背光已关)。熄屏与"熄屏后按任意键只亮屏、不执行动作"
// 这两条行为只能从背光上看出来,串口 status 用它把状态摆出来。
bool love_app_screen_off(void);

// 距最后一次按键过了多少秒,以及自动深睡的阈值(秒)。
// 深睡要同时满足三道闸门(按键静了、网页静了、蓝牙没连着),只看"屏幕熄了"回答不了
// "它为什么不睡" —— 最容易被忽略的两道是手机后台页还在轮询、以及熄屏档位被设成了常亮
// (常亮按设计同时关掉深睡)。串口 status 把这几个数一起摆出来,让这个问题可以被读出来。
uint32_t love_app_idle_seconds(void);
uint32_t love_app_deep_sleep_after_seconds(void);

// 当前的自动熄屏档位(秒;0 = 常亮,同时意味着不会自动深睡)。
uint32_t love_app_blank_off_seconds(void);

// 调试模式:开着时设备不熄屏、不自动深睡,直到明确调用 love_app_set_debug(false)
// (状态存 NVS,重启后仍然有效)。插着线调设备时用。
bool love_app_debug_mode(void);
void love_app_set_debug(bool on);

// 请求一次"机身确认":屏幕弹出确认页,短按确定 = 允许,长按确定或超时 = 拒绝。
// 返回 true 表示允许执行。**可以在非 LVGL 任务上调用**(内部自己取锁),
// 调用方会被阻塞到有结论(最多 timeout_ms + 0.5 秒)。
// 蓝牙链路上的危险命令(改 Wi-Fi、关热点、改时间)用它 —— 蓝牙近场但无需配对,
// USB 侧不要求确认(插着线本身就是物理接触)。
bool love_app_confirm_request(const char *action, uint32_t timeout_ms);

// 开关蓝牙串口,并把开关状态写回配置(s_cfg 是本文件的静态变量,外部改不了)。
// **必须在 LVGL 锁外调用**:关蓝牙要等 NimBLE host 任务退出。
// 供控制台的 ble 命令、空闲自动关闭、以及设置页那个开关共用。
esp_err_t love_app_set_ble(bool on);
