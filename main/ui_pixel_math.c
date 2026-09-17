#include "ui_pixel_math.h"

int ui_pixel_blink_frame(uint32_t elapsed_ms)
{
    uint32_t phase = elapsed_ms % 2000U;
    return phase >= 1650U && phase < 1800U;
}

int ui_pixel_jump_offset(unsigned frame)
{
    static const int offsets[] = { 0, -3, -5, -3, 0 };
    return frame < sizeof(offsets) / sizeof(offsets[0]) ? offsets[frame] : 0;
}

int ui_pixel_corner_cut(int x, int y, int w, int h, int radius)
{
    // 半径超过半宽/半高时角落方块会互相重叠,判定式不再成立,直接不切。
    if (radius <= 0 || radius * 2 > w || radius * 2 > h) return 0;
    if (x < 0 || y < 0 || x >= w || y >= h) return 0;

    // arc_* 只在"确实落在角落带里"时才是圆心坐标(恒为正),否则留 -1 当哨兵。
    // 注意不能拿下面 dx/dy 的正负当哨兵:它们是相对圆心的偏移,本来就可正可负。
    const int arc_x = (x < radius) ? radius
                    : ((x >= w - radius) ? (w - radius) : -1);
    const int arc_y = (y < radius) ? radius
                    : ((y >= h - radius) ? (h - radius) : -1);
    if (arc_x < 0 || arc_y < 0) return 0;

    // 两边同乘 4,用整数比较:像素中心 (2x+1, 2y+1) 到弧心 (2*arc, 2*arc)
    // 的平方距离 > (2*radius)²,等价于浮点的 (x+0.5-arc)² + … > radius²。
    const int dx = 2 * x + 1 - 2 * arc_x;
    const int dy = 2 * y + 1 - 2 * arc_y;
    return dx * dx + dy * dy > 4 * radius * radius;
}
