// main/power_sleep.h —— 浅睡眠 / 深睡眠(无界面)。
//
// 从原来的 demo_low_power 页抽出来:睡眠时序本身与界面无关,但深睡眠关外设的
// 顺序是有硬件契约的(见 tests/test_deep_sleep_contract.py),不适合跟着 demo 一起删。
// 因此这里只保留工作线程与时序,界面由 love_app 的本机状态页负责。
//
// 唤醒方式只有 RTC 定时器:仓库尚无板级按键唤醒电路的证据。
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

// RTC 定时器唤醒时长。状态页的操作项文案从这里派生,避免两处各写一个数字。
#define POWER_SLEEP_LIGHT_SECONDS 2
#define POWER_SLEEP_DEEP_SECONDS  5

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
esp_err_t power_sleep_deep(void);

// 是否有一次休眠正在进行(界面据此忽略按键)。
bool power_sleep_busy(void);

// 最近一次浅睡眠的结果快照。
void power_sleep_get_result(power_sleep_result_t *out);

// 深睡眠累计次数(存 RTC 内存,重启后仍可读)。
uint32_t power_sleep_deep_count(void);

// 本次启动是否由深睡眠的 RTC 定时器唤醒。
bool power_sleep_woke_from_deep(void);
