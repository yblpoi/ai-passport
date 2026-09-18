// tests/test_love_key.c —— 按键意图表的主机测试。
//
// 这里钉的是一个**真机上真实发生过**的 bug:一次物理按键会发 PRESS + CLICK 两个事件,
// 若 PRESS 在熄屏时被当成"唤醒",同一次手势的 CLICK 就会紧接着把动作执行掉 ——
// 现象是「熄屏后按上/下,亮了屏还翻了页」。控制台注入只有一个事件,复现不出来。
#include <assert.h>
#include <stdio.h>

#include "love_key.h"

// 官方组件在三种手势下发给应用的事件序列(iot_button.c,细节见 love_key.h)。
static const bsp_btn_ev_t SHORT_PRESS[] = { BSP_BTN_PRESS, BSP_BTN_CLICK };
static const bsp_btn_ev_t DOUBLE_TAP[]  = { BSP_BTN_PRESS, BSP_BTN_PRESS, BSP_BTN_DOUBLE };
static const bsp_btn_ev_t LONG_PRESS[]  = { BSP_BTN_PRESS, BSP_BTN_LONG };

typedef struct {
    const bsp_btn_ev_t *events;
    size_t count;
    const char *name;
} gesture_t;

static const gesture_t GESTURES[] = {
    { SHORT_PRESS, 2, "短按" },
    { DOUBLE_TAP, 3, "连按两次" },
    { LONG_PRESS, 2, "长按" },
};

// 走一遍手势,统计"亮屏"与"执行动作"各发生几次。
static void run_gesture(bool *screen_off, const gesture_t *gesture, int *wake, int *act)
{
    for (size_t i = 0; i < gesture->count; i++) {
        switch (love_key_intent(*screen_off, gesture->events[i])) {
        case LOVE_KEY_WAKE:
            (*wake)++;
            *screen_off = false;   // 亮屏之后就不再是熄屏状态
            break;
        case LOVE_KEY_ACT:
            (*act)++;
            break;
        case LOVE_KEY_IGNORE:
            break;
        }
    }
}

// 熄屏时:按下什么都没发生;三个判定事件都只亮屏、不下发动到界面。
static void test_blanked_screen_never_acts(void)
{
    assert(love_key_intent(true, BSP_BTN_PRESS) == LOVE_KEY_IGNORE);
    assert(love_key_intent(true, BSP_BTN_CLICK) == LOVE_KEY_WAKE);
    assert(love_key_intent(true, BSP_BTN_DOUBLE) == LOVE_KEY_WAKE);
    assert(love_key_intent(true, BSP_BTN_LONG) == LOVE_KEY_WAKE);
}

// 亮屏时:按下仍然不是动作(否则同一次手势会被执行两遍),判定事件正常下发。
static void test_awake_screen_acts_on_the_judged_event(void)
{
    assert(love_key_intent(false, BSP_BTN_PRESS) == LOVE_KEY_IGNORE);
    assert(love_key_intent(false, BSP_BTN_CLICK) == LOVE_KEY_ACT);
    assert(love_key_intent(false, BSP_BTN_DOUBLE) == LOVE_KEY_ACT);
    assert(love_key_intent(false, BSP_BTN_LONG) == LOVE_KEY_ACT);
}

// 三种手势在熄屏下都**只亮屏一次、不执行任何动作**;紧接着的下一次手势要正常执行
// (别把"唤醒那一下"变成永久屏蔽)。
static void test_gestures_wake_once_then_act_next_time(void)
{
    for (size_t i = 0; i < sizeof(GESTURES) / sizeof(GESTURES[0]); i++) {
        bool screen_off = true;
        int wake = 0, act = 0;
        run_gesture(&screen_off, &GESTURES[i], &wake, &act);
        if (wake != 1 || act != 0) {
            printf("手势 %s(熄屏):亮屏 %d 次,执行 %d 次(期望 1 / 0)\n",
                   GESTURES[i].name, wake, act);
        }
        assert(wake == 1);
        assert(act == 0);

        int wake2 = 0, act2 = 0;
        run_gesture(&screen_off, &GESTURES[i], &wake2, &act2);
        if (wake2 != 0 || act2 != 1) {
            printf("手势 %s(已亮屏):亮屏 %d 次,执行 %d 次(期望 0 / 1)\n",
                   GESTURES[i].name, wake2, act2);
        }
        assert(wake2 == 0);
        assert(act2 == 1);
    }
}

int main(void)
{
    test_blanked_screen_never_acts();
    test_awake_screen_acts_on_the_judged_event();
    test_gestures_wake_once_then_act_next_time();

    printf("test_love_key: PASS\n");
    return 0;
}
