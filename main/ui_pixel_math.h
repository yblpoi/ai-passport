#pragma once

#include <stddef.h>
#include <stdint.h>

// 圆角遮罩:返回非 0 表示像素 (x, y) 落在圆角矩形之外,应当透明。
//
// 判定用像素中心到该角圆弧圆心的距离:弧心在 (radius, radius) /
// (w - radius, radius) / …,像素既落在角落方块内、距离又大于 radius 就切掉。
//
// 四个角必须完全对称。实现刻意把"是否在角落"和"距离"分成两个变量 ——
// 早先的版本用负数当"不在角落"的哨兵,而左侧/上侧算出来的距离本身就是负数,
// 于是左上、右上、左下三个角被一起跳过,设备上只有右下角被切。
int ui_pixel_corner_cut(int x, int y, int w, int h, int radius);

// I4 图的数据开头固定是 16 个调色板项,每项 4 字节(内存顺序 B,G,R,A)。
#define UI_PIXEL_I4_PALETTE_BYTES 64

// 把一张 4bpp 索引图(每字节 2 像素、高半字节在前)打包成 LVGL 的 I4 图:调色板 16 项
// 在前、索引数据在后。这正是内置图标(assets/images/love_pixel_art.c)与 lv_bin_decoder
// 的约定(索引格式的 palette 取 image->data 开头)。
//
// 为什么要打包而不是解成 ARGB8888:一屏四张头像 ARGB 要 25.6KB 常驻,而 I4 每张只占
// 64 + w*h/2 字节(40x40 时 864 字节)。四角镂空的做法是:找一个**这张图没用到的**
// 调色板索引,把它的 alpha 置 0,再把四角像素改指到它;16 色全用满时退而用"用得最少"
// 的那个索引,并把它原来的像素改指到最近的另一种颜色(损失只落在那一小撮像素上)。
//
// out 需要 UI_PIXEL_I4_PALETTE_BYTES + w*h/2 字节;palette 是 16 个 0xRRGGBB。
// 返回用来表示透明的索引(0..15),参数非法或缓冲不够时返回 -1 —— 调用方据此退回内置图标。
int ui_pixel_pack_avatar_i4(uint8_t *out, size_t out_size, const uint8_t *packed,
                            const uint32_t *palette, int w, int h, int cut_radius);
