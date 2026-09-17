// main/love_app.h —— 像素风恋爱倒计时的应用入口。
//
// 开机直接进入本应用,没有中间菜单。三个视图:
//   主屏  在一起 N 天 + 两个人物
//   事件卡 上/下 逐个切换事件与倒计时
//   设置页 长按确定键进入;可开关后台热点、对时、改熄屏档位、看本机状态
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

void love_app_enter(void);
void love_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);
esp_err_t love_app_start(void);
esp_err_t love_app_stop(void);
