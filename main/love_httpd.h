// main/love_httpd.h —— 后台管理页与 REST 接口。
//
// 网页在热点 192.168.4.1 上提供服务,内容由 tools/gen_admin_page.py 从
// assets/web/admin.html 生成,和设备的像素素材、配色完全一致。
#pragma once

#include "esp_err.h"

#include <stdbool.h>

// 配置被后台改动后回调(在 HTTP 任务上下文执行),应用用它刷新设备界面。
typedef void (*love_httpd_changed_cb_t)(void);
void love_httpd_set_changed_cb(love_httpd_changed_cb_t cb);

esp_err_t love_httpd_start(void);
void love_httpd_stop(void);
