// main/power_sleep.h —— 浅睡眠 / 深睡眠(无界面)。
//
// 从原来的 demo_low_power 页抽出来:睡眠时序本身与界面无关,但深睡眠关外设的
// 顺序是有硬件契约的(见 tests/test_deep_sleep_contract.py),不适合跟着 demo 一起删。
// 因此这里只保留工作线程与时序,界面由 love_app 的本机状态页负责。
//
// 深睡的唤醒源有两个,可以只用其中一个:
//   - 机身三个按键任意一个(低电平,共用 GPIO0)—— 见 power_sleep.c 的 arm_button_wake();
//     idle 路径**只**用它,即"睡到有人按键为止"。
//   - RTC 定时器 —— 只有状态页那条手动操作与串口 `sleep deep <秒>` 会设它,
//     用来把深睡变成一件可以在主机上确定观察到的事。
// 定时器唤醒会让设备自己反复重启(重启循环),不是"关机",所以 idle 路径不设它。
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

// RTC 定时器唤醒时长。状态页的操作项文案从这里派生,避免两处各写一个数字。
#define POWER_SLEEP_LIGHT_SECONDS 2
#define POWER_SLEEP_DEEP_SECONDS  5

// 请求深睡之后,等多久来判定"这次到底睡没睡成"。工作线程只在**唤醒源武装失败**时
// 把 power_sleep_busy() 放下来(成功就不会返回),而武装是入睡前最早的几步之一,
// 所以这个窗口不用长 —— 它的用途是让调用方能把已经停掉的 Wi-Fi/BLE/HTTP 起回来
// (见 love_app_sleep_deep)。
#define POWER_SLEEP_ARM_GRACE_MS 1000

// 最近一次休眠的结果。深睡眠成功时不返回(会重启),所以只有浅睡眠与
// 「深睡眠请求失败」两种情况下这里才有内容。
typedef struct {
    bool attempted;      // 是否执行过
    bool deep;           // true = 深睡眠请求
    bool ok;             // 是否成功
    int32_t slept_ms;    // 实际睡了多久(仅浅睡眠有意义)
    esp_err_t error;     // 失败时的错误码
} power_sleep_result_t;

// 请求一次浅睡眠:入睡前 suspend ES8311 并关背光,RTC 定时器唤醒后恢复。
// 工作线程由本模块懒创建,调用后立即返回,进度用 power_sleep_busy() 查。
esp_err_t power_sleep_light(void);

// 请求一次深睡眠:按 CW2017 -> ES8311 -> I2S -> 共享 I2C -> LCD 的顺序停外设后重启。
// 调用方必须先停掉 Wi-Fi / BLE / HTTP 等持有外设的服务。
// 唤醒时长用 POWER_SLEEP_DEEP_SECONDS(状态页那个"深睡眠 N 秒"的操作项)。
esp_err_t power_sleep_deep(void);

// 同上,但自定 RTC 定时器唤醒时长(秒)。给"熄屏后长时间无人操作自动深睡"与串口调试用。
//
// **wake_seconds = 0 表示不设定时器**:只用机身按键唤醒,也就是"睡到有人按键为止"。
// 这是 idle 路径要的形态 —— 定时唤醒会让设备每 N 秒自己重启一次(重启循环),不是休眠。
//
// 返回值只说"请求有没有送进工作线程",不代表已经睡着;真正没睡成(唤醒源武装失败)时,
// power_sleep_busy() 会在一小段时间内落回 false —— 调用方靠它判失败并恢复界面,
// 见 love_app_sleep_deep()。
esp_err_t power_sleep_deep_for(uint32_t wake_seconds);

// 是否有一次休眠正在进行(界面据此忽略按键)。
bool power_sleep_busy(void);

// 本次深睡是否已经**过了会失败的那一段**(唤醒源武装好、开始关外设)。
// 深睡唯一会失败的步骤就是武装唤醒源(run_deep_sleep 的头几步),过了那一步之后只有
// "睡下去"和"重启"两种结局。调用方(见 love_app_sleep_deep)靠它区分:
//   committed 变真  → 正在入睡,不会再返回;
//   busy 落了而 committed 仍是假 → 确实没睡成,该把已停掉的服务起回来。
// 为什么要这个标志而不是只看 busy:武装本身也可能慢到超出宽限窗口,那时"busy 还在"
// 说明不了任何事 —— 猜错会把"网停了、觉也没睡成"留给用户。
bool power_sleep_committed(void);

// 最近一次浅睡眠的结果快照。
void power_sleep_get_result(power_sleep_result_t *out);

// 深睡眠累计次数(存 RTC 内存,重启后仍可读)。
uint32_t power_sleep_deep_count(void);

// 本次启动是否由深睡眠的 RTC 定时器唤醒。
bool power_sleep_woke_from_deep(void);

// 本次启动的唤醒原因(中文短文案):"上电/复位""定时唤醒""按键唤醒"等。
// 深睡时 USB-Serial-JTAG 是断电的,启动最早那几行日志主机常常接不住,所以这个原因
// 只能靠接口问,而不是靠翻日志 —— 串口 status 会把它打出来。
const char *power_sleep_wake_text(void);

// 最近一次"由深睡眠唤醒"的原因文案;从没被深睡唤醒过时返回 NULL。
// 为什么要单独存一份:唤醒原因寄存器会被**任何一次复位**清成"上电/复位",而主机插着
// USB 时开一次串口就可能复位芯片(实测 rst:0x15 USB_UART_CHIP_RESET)。抄进 RTC 内存
// 之后,即使随后被复位,串口仍然答得出"刚才是谁把我叫醒的"。
const char *power_sleep_last_deep_wake_text(void);

// 启动早期调一次(app_main 里):把"本次是谁唤醒的"抄进 RTC 内存。
// 为什么需要抄:唤醒原因寄存器会被**任何一次复位**清成"上电/复位",而主机插着 USB 时
// 开一次串口就可能复位芯片(实测 rst:0x15 USB_UART_CHIP_RESET)。抄一份之后,即使随后
// 被复位,串口仍然答得出"刚才是谁把我叫醒的"。
void power_sleep_note_boot(void);
