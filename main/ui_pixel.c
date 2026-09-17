// main/ui_pixel.c —— 像素风控件的最小集合。
//
// 只保留应用真正在用的四个:纯色块、卡片、标签与它们的公共底座。
// 早期还有一整套 demo 场景(天空/草地/云、小电视吉祥物、菜单选中态),
// 随 demo 子系统一起删掉了 —— 需要时从 git 历史里取。
#include "ui_pixel.h"

// 像素风控件的公共底座:去掉滚动、边框和内边距。
// 圆角、背景色、背景透明度、位置与尺寸都留给调用方,避免在这里替它们做决定。
lv_obj_t *ui_pixel_plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

lv_obj_t *ui_pixel_block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = ui_pixel_plain(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color)
{
    ui_pixel_block(parent, x + 5, y + 6, w, h, UI_INK);
    lv_obj_t *panel = ui_pixel_block(parent, x, y, w, h, color);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(panel, 4, 0);
    lv_obj_set_style_pad_all(panel, 7, 0);
    return panel;
}
