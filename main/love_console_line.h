// main/love_console_line.h —— 串口控制台的行协议(纯逻辑,不依赖 ESP-IDF)。
//
// USB 与 BLE 两条链路喂进来的是**字节流**,不是行:一次 BLE 写入可能只有半行,
// 也可能一次带好几行;"回车"在手机 App 上有 \n、\r\n、\r 三种写法。这一层只负责
// 把字节流还原成整行、再把整行切成参数,与具体传输无关,所以能主机测试
// (见 tests/test_love_console_line.c)。
#pragma once

#include <stdbool.h>
#include <stddef.h>

// 一行命令的最大长度(含结尾 '\0')。与 USB 侧 esp_console 配的
// max_cmdline_length 对齐,免得同一个命令在两个入口上长度限制不一样。
#define LOVE_LINE_MAX 128

// 一行最多切出几个参数(含命令名)。最长的真实命令是
// 「wifi <32 字节 SSID> <64 字节密码>」共 3 个,留到 8 是为了以后加命令不必改协议。
#define LOVE_ARGV_MAX 8

// 字节流 → 整行。调用方持有一个实例,每收到一段字节就逐字节喂给 love_line_feed()。
typedef struct {
    char buf[LOVE_LINE_MAX];   // 始终以 '\0' 结尾,可直接当 C 字符串用
    size_t len;
    // 本行已经超长,后面的字节全部丢弃,直到行尾。**半截命令绝不能被当成完整
    // 命令行执行** —— 否则一个被截断的 "wifi xxx 密码前一半" 会当成合法凭据写进 NVS。
    bool overflow;
    bool skip_lf;              // 上一行以 '\r' 结束,紧跟的 '\n' 属于同一个行尾
    // buf 里有一整行还没被取走。置位是为了让"返回 true 之后缓冲区仍然可读";
    // 下一个字节到来时自动开始新行,调用方不必记得手动复位。
    bool pending;
} love_line_t;

// 行解析结果。刻意把"空行"与"参数过多"分开:前者什么都不做,后者必须报错 ——
// 截断后执行会变成"少了密码的 wifi 命令"这种静默错误。
typedef enum {
    LOVE_CMD_EMPTY = 0,      // 空行,或只有空白
    LOVE_CMD_OK,             // argv[0..argc-1] 可用
    LOVE_CMD_TOO_MANY_ARGS,  // 超过 argv_max 个参数,拒绝执行
} love_cmd_status_t;

void love_line_reset(love_line_t *line);

// 喂入一个字节。返回 true 表示 buf 里已经攒成一整行(不含行尾符),可以读走并执行了;
// 取走后不必手动复位,下一个字节会自动开始收集新的一行。
// 超长行在遇到行尾时**整行丢弃并返回 false**,同时自动开始收集下一行。
bool love_line_feed(love_line_t *line, char c);

// 把一整行**就地**切成参数(空白处写 '\0')。只按空白切分,不处理引号也不支持转义:
// 命令行的约定就是"参数不能含空格",省掉引号解析也就省掉了一整类歧义。
love_cmd_status_t love_line_split(char *text, char *argv[], size_t argv_max, size_t *argc);
