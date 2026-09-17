// components/bsp/include/bsp_button.h
// 三个按键共用一个 ADC 引脚,靠分压电阻区分。电压窗口见 bsp_pins.h。
#pragma once

#include "esp_err.h"

// 按键索引。数量用 bsp_pins.h 的 BSP_BTN_COUNT(硬件属性,归引脚表管),
// 这里不再定义尾项计数,避免出现 BSP_BTN_COUNT / BSP_BTN_COUNT_ 两个近似名字。
typedef enum {
    BSP_BTN_UP = 0,
    BSP_BTN_DOWN,
    BSP_BTN_OK,
} bsp_btn_t;

typedef enum {
    BSP_BTN_PRESS = 0,   // 按下瞬间(低延迟,适合游戏类即时响应)
    BSP_BTN_CLICK,       // 单击(按下并抬起)
    BSP_BTN_DOUBLE,      // 双击
    BSP_BTN_LONG,        // 长按
} bsp_btn_ev_t;

// 按键事件回调。运行于 button 组件使用的共享 esp_timer 任务,只能入队或执行同等级
// 的有界操作；勿在其中阻塞、访问 LVGL 或做重活。
typedef void (*bsp_btn_cb_t)(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

// 成功调用可重复，并更新回调与 user；失败会回滚本次已创建的按键和 ADC 资源。
// ADC 校准失败时返回错误而不是把无效电压解码为按键，修正故障后可重试。
esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user);

// 读当前 ADC 原始电压(mV)。松开时约 3300;按住某键时约为该键的分压值。
// ★ 换了分压/上拉阻值后,用它测出自己的三档电压,再改 bsp_pins.h 的 BSP_BTN_MV_TABLE。
// 读取失败返回 -1。
int bsp_button_read_mv(void);

// 把按键**整套**挂起:停掉周期采样,并拆掉按键与 ADC 单元/校准(见 .c 里的实测说明)。
// **深睡眠前必须调用**:三键共用 GPIO0,而深睡的按键唤醒是"该脚低电平"触发的;
// 只要这个脚还挂在 ADC 的输入网络上,采样与输入网络就会给这个节点注入瞬变,而唤醒源
// 盯的就是电平本身 —— 顺序反了就会睡下去立刻被自己叫醒(实测请求睡 120 秒、约 2 秒后
// 醒来,原因报"按键唤醒")。
// 唤醒源武装失败时调用方会立刻 bsp_button_resume() 把它装回来(见 power_sleep.c:
// "没配上唤醒源就不睡"),所以恢复路径必须有、且要能重建回调。
esp_err_t bsp_button_suspend(void);

// 把 bsp_button_suspend() 拆掉的东西按原回调重建回来。
esp_err_t bsp_button_resume(void);
