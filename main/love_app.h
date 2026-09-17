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

void love_app_enter(void);
void love_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);
esp_err_t love_app_start(void);
esp_err_t love_app_stop(void);


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
