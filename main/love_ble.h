// main/love_ble.h —— 蓝牙串口控制台(Nordic UART Service)。
//
// 设备广播成 LoveCount-XXXX,手机用任意 BLE 串口 App(Serial Bluetooth Terminal、
// nRF Connect 等)连上,就能像 USB 串口那样敲命令(见 love_console.h)。用标准 NUS
// UUID 是为了让现成 App 不用手配 UUID。
//
// 不做 Wi-Fi 配网也不做对时:配网走这里敲 wifi 命令或走后台热点网页,
// 对时走 SNTP、网页,或者这里的 time 命令。
//
// 蓝牙默认关闭(配置里的 ble_enabled),开启后无人连接满 5 分钟会自动关闭并写回配置。
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 广播名形如 LoveCount-XXXX(XXXX 取自 MAC 后两字节)。
void love_ble_device_name(char *buf, size_t size);

esp_err_t love_ble_start(void);
esp_err_t love_ble_stop(void);

// 协议栈是否已经起来(包含"已启动但还没开始广播"的短暂状态)。
// 要决定"该不该 start/stop"时用这个 —— 别用广播状态,启动途中它是 false。
bool love_ble_running(void);

// 当前是否有手机连着。
bool love_ble_connected(void);

// 状态中文短文案(未开启/启动中/广播中/已连接)。界面与 status 命令共用一张表。
const char *love_ble_state_text(void);

// 无人连接的秒数;有连接时返回 0。满 5 分钟就自动关闭。
uint32_t love_ble_idle_seconds(void);

// 关蓝牙的请求:由**应用层**执行(要写回配置、刷新界面)。空闲超时与
// "从 BLE 链路敲 ble off"都会走它。注册方必须自己保证线程安全。
void love_ble_set_shutdown_cb(void (*fn)(void));
void love_ble_request_stop(void);

// NimBLE host 任务的剩余栈(字节);没在跑时返回 0。用于确认 host 栈没有配得太小。
size_t love_ble_host_stack_headroom(void);
