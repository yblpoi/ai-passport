#pragma once

#include <stdint.h>

int ui_pixel_blink_frame(uint32_t elapsed_ms);
int ui_pixel_jump_offset(unsigned frame);

// 圆角遮罩:返回非 0 表示像素 (x, y) 落在圆角矩形之外,应当透明。
//
// 判定用像素中心到该角圆弧圆心的距离:弧心在 (radius, radius) /
// (w - radius, radius) / …,像素既落在角落方块内、距离又大于 radius 就切掉。
//
// 四个角必须完全对称。实现刻意把"是否在角落"和"距离"分成两个变量 ——
// 早先的版本用负数当"不在角落"的哨兵,而左侧/上侧算出来的距离本身就是负数,
// 于是左上、右上、左下三个角被一起跳过,设备上只有右下角被切。
int ui_pixel_corner_cut(int x, int y, int w, int h, int radius);
