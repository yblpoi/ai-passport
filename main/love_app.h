// main/love_app.h —— 像素风恋爱倒计时的应用入口(宿主应用)。
//
// 开机直接进入本应用;长按确定键进入设置页,设置页里可以打开后台热点、
// 查看时间/网络状态,或跳到 demo 菜单。接口与 demo_entry_t 一致,
// 因此它也能作为菜单里的一项被正常进入和退出。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

void love_app_enter(void);
void love_app_exit(void);
void love_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);
esp_err_t love_app_start(void);
esp_err_t love_app_stop(void);
