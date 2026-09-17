#pragma once

#include "lvgl.h"

#define UI_INK        0x17202A
#define UI_PAPER      0xF4F4EA

// 像素控件的公共底座(去滚动/边框/内边距);圆角与背景留给调用方。
lv_obj_t *ui_pixel_plain(lv_obj_t *parent);
// 一个纯色像素块:在 ui_pixel_plain 基础上定位置、尺寸与背景色。
lv_obj_t *ui_pixel_block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color);
// 一张像素卡片:墨色投影 + 指定底色 + 墨色粗边 + 7px 内边距。
lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color);
lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t color);
