// main/love_httpd.h —— 后台管理页与 REST 接口。
//
// 网页在热点 192.168.4.1 上提供服务,内容由 tools/gen_admin_page.py 从
// assets/web/admin.{html,css,js} 生成,和设备的像素素材、配色完全一致。
// 页面、样式、脚本与底纹各走一条路由(见 love_httpd.c 的 URIS),而不是全部
// 内联进 HTML —— 内联的那份 122KB 像素字体曾让单次响应到 210KB,设备发不完。
// 图标反过来保持内联:它们只有两千多字节,拆成独立请求反而多开一堆连接。
#pragma once

#include "esp_err.h"

#include <stdbool.h>

// 配置被后台改动后回调(在 HTTP 任务上下文执行),应用用它刷新设备界面。
typedef void (*love_httpd_changed_cb_t)(void);
void love_httpd_set_changed_cb(love_httpd_changed_cb_t cb);

esp_err_t love_httpd_start(void);
void love_httpd_stop(void);
