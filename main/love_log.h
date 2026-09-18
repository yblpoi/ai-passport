// main/love_log.h —— 日志级别的唯一策略点:串口 `log` 命令与截屏静默都走这里。
//
// 为什么不让调用方各自调 esp_log_level_set:IDF 的 `set("*", level)` 会**顺手清掉
// 所有按 TAG 的覆盖**(见 components/log/src/log_level/tag_log_level/tag_log_level.c)。
// 截屏正是那么静默的 —— 谁自己设过两下,谁就被截屏悄悄改回全局级别。策略集中在这里
// 之后,截屏改成 mute/unmute,按 TAG 的设置在窗口结束后会被重新装回去。
#pragma once

#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <stddef.h>

// TAG 名上限(含结尾 NUL)与能同时记住的覆盖条数。真实 TAG 最长的是 love_console(12),
// 16 够用;表满了 `log <模块> <级别>` 会明确报错,而不是悄悄丢掉一条。
#define LOVE_LOG_TAG_MAX 16
#define LOVE_LOG_TAG_COUNT 8

// 编译期上限(CONFIG_LOG_MAXIMUM_LEVEL):比它更啰嗦的级别在编译期就被裁掉了,
// 设了也不会生效,所以设置接口会拒绝而不是静默接受。
#define LOVE_LOG_BUILD_MAX ((esp_log_level_t)CONFIG_LOG_MAXIMUM_LEVEL)

typedef struct {
    char tag[LOVE_LOG_TAG_MAX];
    esp_log_level_t level;
} love_log_override_t;

// 装上默认策略(幂等)。要在其它模块开始打日志之前调用:
// 默认把 wifi/wpa 两个驱动 TAG 降到 warn —— 连不上网时它们按帧刷屏,而这两条
// 信息对"设备当前是什么状态"没有增量(候选切换与最终失败由 love_net 自己报)。
esp_err_t love_log_init(void);

// 全局级别。注意它与按 TAG 的覆盖是两回事:某个 TAG 有覆盖时以覆盖为准。
esp_log_level_t love_log_global_level(void);

// 设置全局级别。高于编译期上限(CONFIG_LOG_MAXIMUM_LEVEL)时返回 ESP_ERR_NOT_SUPPORTED
// —— 那种级别在编译期就被裁掉了,静默接受只会让用户以为生效了。
esp_err_t love_log_set_global(esp_log_level_t level);

// 设置/更新一个 TAG 的覆盖。表满返回 ESP_ERR_NO_MEM(这里只有"放不下"一种失败,
// 不去发明一个 IDF 里没有的错误码)。
esp_err_t love_log_set_tag(const char *tag, esp_log_level_t level);

// 读出当前全部覆盖(默认策略 + 运行时改过的),返回条数。
size_t love_log_overrides(love_log_override_t *out, size_t max);

// 恢复成默认策略(丢弃运行时改过的每一条)。
void love_log_reset(void);

// 截屏用:窗口内一个日志字节都不能混进二进制流。mute 之后到 unmute 之前的
// set_global/set_tag 只改表、不生效 —— 否则窗口内冒出一条日志就毁掉整幅图。
void love_log_mute_all(void);
void love_log_unmute_all(void);

// 级别名与解析。解析层面认出 none/off/error/warn/info/debug/verbose —— 认 debug/verbose
// 不是为了能设它们(编译期上限以上就是没有),而是为了在用户敲 `log debug` 时能回一句
// "比固件里编进来的上限更啰嗦",而不是含糊的"用法错误"。
const char *love_log_level_name(esp_log_level_t level);
bool love_log_level_parse(const char *text, esp_log_level_t *out);
