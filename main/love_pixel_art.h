// main/love_pixel_art.h —— 由 assets/images/love_pixel_art_gen.py 生成,请勿手改。
// 像素图标与爱心底纹:设备界面与后台网页共用同一份素材。
#pragma once

#include "lvgl.h"

// 图标序号,与后台网页的图标选择顺序一致。
#define LOVE_ICON_BIRD 0  // 小鸟
#define LOVE_ICON_CAT 1  // 猫咪
#define LOVE_ICON_DOG 2  // 狗狗
#define LOVE_ICON_RABBIT 3  // 兔子
#define LOVE_ICON_BEAR 4  // 小熊
#define LOVE_ICON_FOX 5  // 狐狸
#define LOVE_ICON_HEART 6  // 爱心
#define LOVE_ICON_STAR 7  // 星星
#define LOVE_ICON_FLOWER 8  // 小花
#define LOVE_ICON_MOON 9  // 月亮
#define LOVE_ICON_CAKE 10  // 蛋糕
#define LOVE_ICON_GIFT 11  // 礼物
#define LOVE_ICON_BALLOON 12  // 气球
#define LOVE_ICON_RING 13  // 戒指
#define LOVE_ICON_LEAF 14  // 叶子
#define LOVE_ICON_TREE 15  // 圣诞树

#define LOVE_ICON_COUNT 16
#define LOVE_ICON_PX 40
#define LOVE_BG_TILE_PX 48

// 16 色调色板,顺序即自定义头像的 4bpp 索引顺序。
// 后台网页按同一张表量化上传的图片,所以两端颜色是同一套。
#define LOVE_PALETTE_COUNT 16
extern const uint32_t love_pixel_palette[LOVE_PALETTE_COUNT];

// 越界时返回第 0 个图标,不返回 NULL,调用方无需判空。
const lv_image_dsc_t *love_pixel_icon(uint8_t index);
const lv_image_dsc_t *love_pixel_bg_tile(void);
