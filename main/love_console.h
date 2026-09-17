// main/love_console.h —— USB 串口命令行(配网用)。
//
// 后台网页和 BLE 对时之外多一条入口:不用连热点、不用装 App,插上 USB 就能配网。
// 三条链路并存,谁也不替代谁。
#pragma once

#include "esp_err.h"

// 在 USB-Serial-JTAG 控制台上启动 REPL。失败只影响这条入口,调用方无需致命处理。
esp_err_t love_console_start(void);
