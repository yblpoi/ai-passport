// main/love_key.c —— 按键意图判定,规则见 love_key.h。
#include "love_key.h"

love_key_intent_t love_key_intent(bool screen_off, bsp_btn_ev_t ev)
{
    // 按下瞬间只算"有人动了"(调用方据此喂住熄屏/深睡计时),不是一次按键动作。
    // **熄屏时也一样**:它只是同一次手势的开头,后面那个判定事件才是用户要做的事。
    if (ev == BSP_BTN_PRESS) return LOVE_KEY_IGNORE;

    // 熄屏后的第一个判定事件只负责亮屏 —— 这是产品规则,不是顺手:用户想看一眼
    // 天数时,那一按不该把页面翻走、更不该改到设置。
    if (screen_off) return LOVE_KEY_WAKE;

    return LOVE_KEY_ACT;
}
