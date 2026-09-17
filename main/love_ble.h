// main/love_ble.h —— BLE 对时通道。
//
// 设备广播一个自定义 GATT 服务:手机用任意 BLE 调试工具(nRF Connect、
// LightBlue 等)连上后,向“时间”特征写入时间戳即可对时;状态特征会回读当前
// 时间与来源,便于在手机上确认。这里不做 Wi-Fi 配网,配网走后台热点网页。
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 广播名形如 LoveCount-XXXX(XXXX 取自 MAC 后两字节)。
void love_ble_device_name(char *buf, size_t size);

// 服务/特征 UUID(16 位自定义):A001 服务、A002 写入时间、A003 读取状态。
#define LOVE_BLE_SVC_UUID   0xA001
#define LOVE_BLE_TIME_UUID  0xA002
#define LOVE_BLE_STATE_UUID 0xA003

esp_err_t love_ble_start(void);
esp_err_t love_ble_stop(void);
// 是否正在广播。注意启动过程中会短暂为 false。
bool love_ble_ready(void);

// 协议栈是否已经起来(包含"已启动但还没开始广播"的短暂状态)。
// 要决定"该不该 stop"时用这个,别用 love_ble_ready() —— 否则启动途中会漏掉一次关闭。
bool love_ble_running(void);
