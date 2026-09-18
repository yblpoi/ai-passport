#include <assert.h>
#include <stdio.h>
#include "ui_pixel_math.h"

// 头像圆角遮罩。40x40、半径 4 是设备与素材生成器实际用的参数。
#define ICON_PX 40
#define RADIUS 4

// 四个角必须互为镜像。这条断言是针对一个真实缺陷加的:早先的实现用负数当
// "不在角落"的哨兵,而左侧/上侧算出的距离本来就是负数,结果左上、右上、左下
// 三个角被整体跳过,设备上只有右下角被切。任何单侧判定的写法都会在这里失败。
static void test_corner_cut_is_symmetric(void)
{
    for (int y = 0; y < ICON_PX; y++) {
        for (int x = 0; x < ICON_PX; x++) {
            const int cut = ui_pixel_corner_cut(x, y, ICON_PX, ICON_PX, RADIUS);
            assert(ui_pixel_corner_cut(ICON_PX - 1 - x, y, ICON_PX, ICON_PX, RADIUS) == cut);
            assert(ui_pixel_corner_cut(x, ICON_PX - 1 - y, ICON_PX, ICON_PX, RADIUS) == cut);
            assert(ui_pixel_corner_cut(ICON_PX - 1 - x, ICON_PX - 1 - y,
                                       ICON_PX, ICON_PX, RADIUS) == cut);
        }
    }
}

// 每角恰好切掉 3 个像素,四角共 12 个 —— 数字写死,半径或判定式一改就会失败。
static void test_corner_cut_pixel_count(void)
{
    int total = 0;
    for (int y = 0; y < ICON_PX; y++) {
        for (int x = 0; x < ICON_PX; x++) {
            total += ui_pixel_corner_cut(x, y, ICON_PX, ICON_PX, RADIUS);
        }
    }
    assert(total == 12);

    // 左上角被切掉的正是 (0,0)、(1,0)、(0,1),其余角落方块内的像素保留。
    assert(ui_pixel_corner_cut(0, 0, ICON_PX, ICON_PX, RADIUS) == 1);
    assert(ui_pixel_corner_cut(1, 0, ICON_PX, ICON_PX, RADIUS) == 1);
    assert(ui_pixel_corner_cut(0, 1, ICON_PX, ICON_PX, RADIUS) == 1);
    assert(ui_pixel_corner_cut(2, 0, ICON_PX, ICON_PX, RADIUS) == 0);
    assert(ui_pixel_corner_cut(3, 0, ICON_PX, ICON_PX, RADIUS) == 0);
    assert(ui_pixel_corner_cut(1, 1, ICON_PX, ICON_PX, RADIUS) == 0);
    // 另外三个角各对应一个镜像点,顺带确认不是"只有右下角生效"。
    assert(ui_pixel_corner_cut(ICON_PX - 1, 0, ICON_PX, ICON_PX, RADIUS) == 1);
    assert(ui_pixel_corner_cut(0, ICON_PX - 1, ICON_PX, ICON_PX, RADIUS) == 1);
    assert(ui_pixel_corner_cut(ICON_PX - 1, ICON_PX - 1, ICON_PX, ICON_PX, RADIUS) == 1);
}

static void test_corner_cut_degenerate_inputs(void)
{
    // 半径 0 或负数:不切
    assert(ui_pixel_corner_cut(0, 0, ICON_PX, ICON_PX, 0) == 0);
    assert(ui_pixel_corner_cut(0, 0, ICON_PX, ICON_PX, -1) == 0);
    // 半径超过半宽/半高:角落方块会重叠,不切
    assert(ui_pixel_corner_cut(0, 0, 8, 8, 5) == 0);
    assert(ui_pixel_corner_cut(0, 0, 8, 6, 4) == 0);
    // 半径恰好等于半宽是退化但合法的情形:圆角矩形此时正好是一个圆,角像素应当被切
    assert(ui_pixel_corner_cut(0, 0, 8, 8, 4) == 1);
    // 越界坐标:不切
    assert(ui_pixel_corner_cut(-1, 0, ICON_PX, ICON_PX, RADIUS) == 0);
    assert(ui_pixel_corner_cut(0, ICON_PX, ICON_PX, ICON_PX, RADIUS) == 0);
    // 正中间永远不切
    assert(ui_pixel_corner_cut(ICON_PX / 2, ICON_PX / 2, ICON_PX, ICON_PX, RADIUS) == 0);
    // 非正方形按各自边长判定:角上切,中间两侧不切
    assert(ui_pixel_corner_cut(0, 0, 16, 8, 2) == 1);
    assert(ui_pixel_corner_cut(0, 4, 16, 8, 2) == 0);
    assert(ui_pixel_corner_cut(8, 0, 16, 8, 2) == 0);
}

// ---------- 自定义头像打包成 I4 ----------
// 这些断言针对一个真机上出现过的问题:列表页一屏 4 个图标,而头像缓存只有 2 个,
// 第 3 个起会静默退回内置图标(张冠李戴)。改成 I4 后一屏 4 张只要 4x864 字节。

#define PALETTE_BYTES UI_PIXEL_I4_PALETTE_BYTES
#define DATA_BYTES    (ICON_PX * ICON_PX / 2)
#define OUT_BYTES     (PALETTE_BYTES + DATA_BYTES)

static uint32_t test_palette[16];
static uint8_t  test_packed[DATA_BYTES];
static uint8_t  test_out[OUT_BYTES];

// 把索引数据铺满某个函数给的索引。fill != NULL 时按 (x+y) 决定用哪个索引。
static void fill_packed(const int *index_of_pixel)
{
    for (int p = 0; p < ICON_PX * ICON_PX; p++) {
        const uint8_t code = (uint8_t)index_of_pixel[p];
        uint8_t *const byte = &test_packed[p / 2];
        *byte = (p % 2 == 0) ? (uint8_t)((code << 4) | (*byte & 0x0F))
                             : (uint8_t)((*byte & 0xF0) | code);
    }
}

static uint8_t index_at(const uint8_t *data, int pixel)
{
    const uint8_t byte = data[pixel / 2];
    return (pixel % 2 == 0) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F);
}

// 只用 0/1 两种索引 → 透明索引应当挑一个没被用到的,调色板里只有它是透明的,
// 四角变成它,而且没有任何内容像素被改动。
static void test_pack_picks_a_free_index(void)
{
    int choice[ICON_PX * ICON_PX];
    for (int p = 0; p < ICON_PX * ICON_PX; p++) choice[p] = (p % 3 == 0) ? 1 : 0;
    fill_packed(choice);

    const int spare = ui_pixel_pack_avatar_i4(test_out, sizeof(test_out), test_packed,
                                              test_palette, ICON_PX, ICON_PX, RADIUS);
    assert(spare >= 2 && spare < 16);          // 0 和 1 都在用
    for (int i = 0; i < 16; i++) {
        assert(test_out[i * 4 + 3] == (i == spare ? 0x00 : 0xFF));
        assert(test_out[i * 4 + 0] == (uint8_t)(test_palette[i] & 0xFF));
        assert(test_out[i * 4 + 1] == (uint8_t)((test_palette[i] >> 8) & 0xFF));
        assert(test_out[i * 4 + 2] == (uint8_t)((test_palette[i] >> 16) & 0xFF));
    }

    const uint8_t *const data = test_out + PALETTE_BYTES;
    int corners = 0;
    for (int y = 0; y < ICON_PX; y++) {
        for (int x = 0; x < ICON_PX; x++) {
            const int p = y * ICON_PX + x;
            if (ui_pixel_corner_cut(x, y, ICON_PX, ICON_PX, RADIUS)) {
                assert(index_at(data, p) == spare);
                corners++;
            } else {
                assert(index_at(data, p) == choice[p]);   // 内容像素一个没动
            }
        }
    }
    assert(corners == 12);      // 与 test_corner_cut_pixel_count 一致
}

// 16 色用满(照片常见)→ 退回"用得最少"的索引,且它原来的像素要被改指到别的颜色,
// 不能在图上留洞。
static void test_pack_all_indices_used_leaves_no_holes(void)
{
    // 0 号铺满,1..15 各出现一次 → 最少的是扫描顺序上第一个 1 次者,即 1 号。
    int choice[ICON_PX * ICON_PX];
    for (int p = 0; p < ICON_PX * ICON_PX; p++) choice[p] = 0;
    for (int i = 1; i < 16; i++) choice[i * 97] = i;
    fill_packed(choice);

    const int spare = ui_pixel_pack_avatar_i4(test_out, sizeof(test_out), test_packed,
                                              test_palette, ICON_PX, ICON_PX, RADIUS);
    assert(spare == 1);
    assert(test_out[spare * 4 + 3] == 0x00);
    for (int i = 0; i < 16; i++) assert(test_out[i * 4 + 3] == (i == spare ? 0x00 : 0xFF));

    const uint8_t *const data = test_out + PALETTE_BYTES;
    int remapped = 0;
    for (int y = 0; y < ICON_PX; y++) {
        for (int x = 0; x < ICON_PX; x++) {
            const int p = y * ICON_PX + x;
            const uint8_t got = index_at(data, p);
            if (ui_pixel_corner_cut(x, y, ICON_PX, ICON_PX, RADIUS)) {
                assert(got == spare);
                continue;
            }
            assert(got != spare);                 // 还有像素指着它 = 图上会有洞
            if (choice[p] == spare) remapped++;   // 这些像素被改指到最近的颜色
            else assert(got == choice[p]);
        }
    }
    assert(remapped == 1);
}

// 参数非法/缓冲不够要明确失败,而不是写出界。
static void test_pack_degenerate_inputs(void)
{
    assert(ui_pixel_pack_avatar_i4(NULL, sizeof(test_out), test_packed, test_palette,
                                   ICON_PX, ICON_PX, RADIUS) == -1);
    assert(ui_pixel_pack_avatar_i4(test_out, sizeof(test_out), NULL, test_palette,
                                   ICON_PX, ICON_PX, RADIUS) == -1);
    assert(ui_pixel_pack_avatar_i4(test_out, sizeof(test_out), test_packed, NULL,
                                   ICON_PX, ICON_PX, RADIUS) == -1);
    assert(ui_pixel_pack_avatar_i4(test_out, sizeof(test_out) - 1, test_packed,
                                   test_palette, ICON_PX, ICON_PX, RADIUS) == -1);
    assert(ui_pixel_pack_avatar_i4(test_out, sizeof(test_out), test_packed, test_palette,
                                   ICON_PX - 1, ICON_PX, RADIUS) == -1);   // 奇数像素
    assert(ui_pixel_pack_avatar_i4(test_out, sizeof(test_out), test_packed, test_palette,
                                   0, ICON_PX, RADIUS) == -1);
    // 半径不合法时 ui_pixel_corner_cut 不切,打包本身仍然成功(不能因此报错)
    assert(ui_pixel_pack_avatar_i4(test_out, sizeof(test_out), test_packed, test_palette,
                                   ICON_PX, ICON_PX, 0) >= 0);
}

int main(void)
{
    for (int i = 0; i < 16; i++) {
        test_palette[i] = (uint32_t)(0x102030u + (uint32_t)i * 0x000407u);
    }
    for (int i = 0; i < DATA_BYTES; i++) test_packed[i] = 0x11;   // 只用索引 1

    test_corner_cut_is_symmetric();
    test_corner_cut_pixel_count();
    test_corner_cut_degenerate_inputs();
    test_pack_picks_a_free_index();
    test_pack_all_indices_used_leaves_no_holes();
    test_pack_degenerate_inputs();

    printf("test_ui_pixel_math: PASS\n");
    return 0;
}
