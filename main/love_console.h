// main/love_console.h —— 命令行控制台,USB 与 BLE 共用一套命令。
//
// 后台网页之外的两条免热点入口:插上 USB 就能敲,或者用手机的 BLE 串口 App 连上
// 敲(见 love_ble.h)。两条链路共享同一张命令表,所以这里只定义"一行怎么执行",
// 不关心它是从哪根线进来的。
//
// 命令的输出同时写 stdout 与已注册的 BLE 出口;stdout 在 love_console_start() 里
// 被设成非阻塞,否则设备只插充电器(没有 USB 主机)时 printf 会把整条链路卡死。
#pragma once

#include "esp_err.h"

#include <stddef.h>

// 命令来自哪条链路。多数命令不关心,但 `ble off` 必须先回话再关蓝牙 ——
// 从 BLE 敲的时候一停栈连接就断了,回复根本没机会发出去。
typedef enum {
    LOVE_CONSOLE_SRC_USB = 0,
    LOVE_CONSOLE_SRC_BLE,
} love_console_src_t;

// 启动 USB 侧 REPL 并注册命令(幂等)。失败只影响 USB 这条入口。
// **必须在 Wi-Fi/BLE 之前调用**:它要一块连续的 4KB 任务栈。
esp_err_t love_console_start(void);

// BLE 串口注册的输出出口(设备发往手机的通知);传 NULL 取消注册。
// 回调在调用命令的那个任务上下文里执行,实现必须自己保证线程安全。
void love_console_set_out(void (*fn)(const char *text, size_t len));

// 执行一整行命令。**会就地修改 line**(按空白写 '\0' 切分)。
// 返回命令自己的退出码:0 成功,非 0 失败;找不到命令返回 127。
int love_console_exec(char *line, love_console_src_t src);

// 输出一行(调用方自己带结尾换行)。一次调用对应一条 BLE 通知,
// 所以不要把半行拆成多次调用,也不要把多行拼成一次。
void love_console_out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
