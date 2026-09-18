#include "bsp_display_rounding.h"

// 一个轴上的"角内偏移":p 落在角列/角行里时返回它到该角圆弧圆心的距离分量,
// 否则返回 -1(角内偏移恒 >= 1,所以 -1 可以当"不在角上"的哨兵)。
// 两个轴用的是同一套判断,分开写迟早会在某一侧漏掉边界(这一层历史上就栽过:
// 用负数当哨兵时左侧/上侧的距离本身就是负数,于是三个角被一起跳过)。
// **必须内联**:这是每个刷屏像素都会走的判定,退化成一个真实调用反而更慢。
static inline int32_t corner_delta(int32_t p, int32_t extent, int32_t radius)
{
    if (p < radius) return radius - p;
    if (p >= extent - radius) return p - (extent - 1 - radius);
    return -1;
}

bool bsp_display_pixel_outside_rounded_rect(int32_t x, int32_t y,
                                            int32_t width, int32_t height,
                                            int32_t radius)
{
    if (x < 0 || y < 0 || x >= width || y >= height) return true;
    if (radius <= 0 || width <= 0 || height <= 0) return false;

    int32_t max_radius = (width < height ? width : height) / 2;
    if (radius > max_radius) radius = max_radius;

    // 判定顺序与原来逐字一致:x 先、y 后,任一侧不在角上就立刻返回 false。
    const int32_t dx = corner_delta(x, width, radius);
    if (dx < 0) return false;
    const int32_t dy = corner_delta(y, height, radius);
    if (dy < 0) return false;

    return dx * dx + dy * dy > radius * radius;
}
