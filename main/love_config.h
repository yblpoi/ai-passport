// main/love_config.h —— 倒计时配置的结构与纯逻辑(不依赖 ESP-IDF/LVGL)。
//
// 为什么单独成文件:配置记录的版本迁移是最危险、最该被回归覆盖的一段逻辑,
// 而 love_store.h 要 include esp_err.h、love_pixel_art.h 要 include lvgl.h,
// 两者都编译不进主机测试。把结构、迁移、字段收敛放在这里,
// tests/test_love_config.c 就能直接喂字节验证。
//
// 不在这里的:love_config_defaults() 留在 love_store.c —— 出厂默认事件要写
// LOVE_ICON_BIRD 这类图标序号,那些宏由生成器写在 love_pixel_art.h 里,
// 而那份头包含 lvgl.h。加载流程本来就是"先铺默认值,再用老记录覆盖",
// 所以迁移函数不需要自己会造默认值。
#pragma once

#include "love_date.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 自动熄屏档位(秒),0 表示常亮。设置页的档位标签、配置校验与后台写入校验
// 共用同一张表,档位有变只需改这里。
#define LOVE_BLANK_OFF_COUNT 5
#define LOVE_BLANK_OFF_DEFAULT 30
extern const uint16_t LOVE_BLANK_OFF_SECONDS[LOVE_BLANK_OFF_COUNT];

// seconds 是否属于已知档位。
bool love_blank_off_valid(uint16_t seconds);

// 自定义头像槽位保存的是"定长 4bpp 索引包":40x40 像素、每字节 2 像素、
// 高半字节在前(顺序与 assets/web/admin.js 的 compressAvatar 一致)。
// 每个槽位固定 LOVE_AVATAR_BYTES 字节,便于按槽位随机读写。
#define LOVE_AVATAR_BYTES 800

// 展示模式。列表 = 按分类分组、分页一屏显示多个事件;单页 = 每个事件独占一屏。
#define LOVE_DISPLAY_LIST 0u
#define LOVE_DISPLAY_PAGE 1u

typedef struct {
    char name[LOVE_NAME_MAX];
    uint8_t icon;
} love_person_t;

typedef struct {
    love_date_t start;                        // 在一起起始日
    love_person_t people[LOVE_PERSON_MAX];    // 主屏顶部的两个人
    uint8_t event_count;                      // 有效事件数,<= LOVE_EVENT_MAX
    uint16_t blank_off_seconds;               // 自动熄屏秒数,0 = 常亮
    love_event_t events[LOVE_EVENT_MAX];      // 事件列表
    // ---- v3 起追加 ----
    // 加在尾部不是为了 memcpy 兼容(events 的元素在 v3 变过,前缀早就不同了),
    // 只是让"对着 v2 结构逐字段读老记录"的迁移代码好写好读。
    uint8_t display_mode;                     // LOVE_DISPLAY_*
    uint8_t ble_enabled;                      // 0 = 关(出厂默认)
} love_config_t;

#define LOVE_CONFIG_VERSION 3u

// 落盘记录:版本号 + 配置。版本号是首字段,任何版本都能先把它读出来再决定怎么解释其余字节。
typedef struct {
    uint32_t version;
    love_config_t config;
} love_config_record_t;

// v2 的布局**冻结**在这里。字段类型与顺序必须保持当年的样子,所以 name 的长度直接写 25,
// 不能用 LOVE_NAME_MAX —— 以后改常量不能篡改"历史格式",否则老设备的记录会被读错。
typedef struct {
    char name[25];
    uint8_t icon;
} love_config_v2_person_t;

typedef struct {
    char name[25];
    uint8_t icon;
    uint8_t kind;        // love_event_kind_t
    love_date_t date;
} love_config_v2_event_t;

typedef struct {
    love_date_t start;
    love_config_v2_person_t people[2];
    uint8_t event_count;
    uint16_t blank_off_seconds;
    love_config_v2_event_t events[8];
} love_config_v2_t;

typedef struct {
    uint32_t version;
    love_config_v2_t config;
} love_config_v2_record_t;

// 复制字符串,并在截断时**不切断多字节字符**(退到最后一个完整字符的边界)。
// 名字、分类名与后台传入的字符串都走这里。
void love_utf8_copy(char *dst, size_t size, const char *src);

// 字段收敛:日期、名字、图标序号、熄屏档位、展示模式、蓝牙开关、事件尾部残留。
// 载入后与落盘前都要跑一遍。
void love_config_sanitize(love_config_t *cfg);

// 把一条落盘记录解释成 v3 配置(含 v2→v3 迁移)。长度与版本都不认识时返回 false,
// 调用方回落到默认值。调用方应先把 out 铺成默认值:迁移只覆盖老记录里真有的字段,
// 新字段(v3 才加的)保持默认。
bool love_config_from_record(const void *blob, size_t size, love_config_t *out);
