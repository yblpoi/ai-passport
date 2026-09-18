// main/love_key.h —— 一个按键事件该怎么处理(纯逻辑,不依赖 ESP-IDF/LVGL)。
//
// 为什么要单独一层:官方 button 组件对**一次物理按键**会发不止一个事件,而"哪一个才算
// 用户想做的事"以前是散在 love_app_key() 里的隐含规则 —— 实测因此漏过一个真机 bug:
// 熄屏时按下发来的 PRESS 先亮了屏,紧接着(松开时)组件判定出的 CLICK 就在"已经亮屏"
// 的状态下把动作执行了,表现为「熄屏后按上/下,亮了屏还翻了页」。控制台 `key` 只注入
// 一个 CLICK,所以这条 bug 在注入路径上永远复现不出来(见 love_console.c 的 cmd_key)。
//
// 官方组件(iot_button.c)对一次按键的事件序列:
//   短按    : PRESS_DOWN →(松开)PRESS_UP →(再等 180ms)SINGLE_CLICK →…→ PRESS_END
//   连按两次: PRESS_DOWN → PRESS_UP → PRESS_DOWN + PRESS_REPEAT → PRESS_UP
//             →(180ms)DOUBLE_CLICK →…→ PRESS_END     ——**不会有 SINGLE_CLICK**
//   长按    : PRESS_DOWN →(1500ms)LONG_PRESS_START →(每 20ms)LONG_PRESS_HOLD
//             →(松开)PRESS_UP / PRESS_END
// 也就是说:一次手势最终只会有一个"判定事件"(CLICK 或 DOUBLE 或 LONG),PRESS 只是
// "手指刚碰到"。上游官方 main 分支的 main.c 也是这个口径 —— navigation_input() 把
// 非 CLICK 的事件一律归成 OTHER 丢掉,只有 CLICK 与确定长按才算输入。
#pragma once

#include "bsp_button.h"

#include <stdbool.h>

typedef enum {
    LOVE_KEY_IGNORE = 0,   // 什么都不做(按下瞬间:它不是一次按键动作)
    LOVE_KEY_WAKE,         // 只亮屏,不把这次按键下发成动作(熄屏后的第一个判定事件)
    LOVE_KEY_ACT,          // 正常交给界面
} love_key_intent_t;

// 判定一次按键事件的意图。screen_off = 当前是否已熄屏(背光已关)。
// 注意:按下(PRESS)在熄屏时也**不是**唤醒事件 —— 让它唤醒就等于把同一次手势的
// CLICK 放进来执行。熄屏唤醒落在 CLICK / DOUBLE / LONG 上。
love_key_intent_t love_key_intent(bool screen_off, bsp_btn_ev_t ev);
