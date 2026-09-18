#include "ui_pixel_math.h"

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

// 与 index 最接近的另一种调色板颜色(按 RGB 距离)。index 的像素要被改指到这里。
static int nearest_other(const uint32_t *palette, int index)
{
    const int pr = (int)((palette[index] >> 16) & 0xFF);
    const int pg = (int)((palette[index] >> 8) & 0xFF);
    const int pb = (int)(palette[index] & 0xFF);

    int best = (index == 0) ? 1 : 0;
    long best_d = -1;
    for (int i = 0; i < 16; i++) {
        if (i == index) continue;
        const int dr = (int)((palette[i] >> 16) & 0xFF) - pr;
        const int dg = (int)((palette[i] >> 8) & 0xFF) - pg;
        const int db = (int)(palette[i] & 0xFF) - pb;
        const long d = (long)dr * dr + (long)dg * dg + (long)db * db;
        if (best_d < 0 || d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

// 把一个像素的 4 位索引写回去(高半字节在前)。
static void put_index(uint8_t *data, int pixel, uint8_t index)
{
    uint8_t *const byte = &data[pixel / 2];
    *byte = (pixel % 2 == 0) ? (uint8_t)((index << 4) | (*byte & 0x0F))
                             : (uint8_t)((*byte & 0xF0) | index);
}

int ui_pixel_pack_avatar_i4(uint8_t *out, size_t out_size, const uint8_t *packed,
                            const uint32_t *palette, int w, int h, int cut_radius)
{
    if (!out || !packed || !palette || w <= 0 || h <= 0) return -1;
    // 4bpp 每行必须是整字节(w/2 字节),否则行与行之间会丢半个字节 —— 宽度为奇数时
    // 整个打包都不成立(像素总数是偶数也不够,例如 39x40)。
    if (w % 2 != 0) return -1;
    const int pixels = w * h;
    if (pixels % 2 != 0) return -1;
    const size_t data_bytes = (size_t)pixels / 2;
    if (out_size < UI_PIXEL_I4_PALETTE_BYTES + data_bytes) return -1;

    // 每个索引用了几次。挑"透明索引"时优先没人用的那个。
    int used[16] = { 0 };
    for (size_t i = 0; i < data_bytes; i++) {
        used[packed[i] >> 4]++;
        used[packed[i] & 0x0F]++;
    }
    int spare = 0;
    for (int i = 0; i < 16; i++) {
        if (used[i] == 0) {
            spare = i;
            break;
        }
        if (used[i] < used[spare]) spare = i;       // 全用满时退而求其次:用得最少的
    }

    for (int i = 0; i < 16; i++) {
        const uint32_t rgb = palette[i];
        out[i * 4 + 0] = (uint8_t)(rgb & 0xFF);          // B
        out[i * 4 + 1] = (uint8_t)((rgb >> 8) & 0xFF);   // G
        out[i * 4 + 2] = (uint8_t)((rgb >> 16) & 0xFF);  // R
        out[i * 4 + 3] = (i == spare) ? 0x00 : 0xFF;     // A
    }

    uint8_t *const data = out + UI_PIXEL_I4_PALETTE_BYTES;
    for (size_t i = 0; i < data_bytes; i++) data[i] = packed[i];

    // 透明索引本来就被用到 → 先把那些像素改指到最近的颜色,别在图上留洞。
    if (used[spare] > 0) {
        const uint8_t to = (uint8_t)nearest_other(palette, spare);
        for (int p = 0; p < pixels; p++) {
            const uint8_t *const byte = &data[p / 2];
            const uint8_t code = (p % 2 == 0) ? (uint8_t)(*byte >> 4)
                                              : (uint8_t)(*byte & 0x0F);
            if (code == spare) put_index(data, p, to);
        }
    }

    // 四角改成透明索引,与内置图标(生成器里把角落写成索引 0)视觉上同一套圆角。
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (ui_pixel_corner_cut(x, y, w, h, cut_radius)) {
                put_index(data, y * w + x, (uint8_t)spare);
            }
        }
    }
    return spare;
}
