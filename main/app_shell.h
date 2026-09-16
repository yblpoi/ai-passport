// main/app_shell.h —— 应用外壳:宿主应用与 demo 菜单之间的切换入口。
#pragma once

// 从宿主应用切到 demo 菜单(会先停止应用持有的 Wi-Fi / BLE / HTTP 服务)。
void app_shell_enter_menu(void);
