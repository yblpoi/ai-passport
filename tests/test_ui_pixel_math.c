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

int main(void)
{
    assert(ui_pixel_blink_frame(0) == 0);
    assert(ui_pixel_blink_frame(1700) == 1);
    assert(ui_pixel_blink_frame(1850) == 0);

    assert(ui_pixel_jump_offset(0) == 0);
    assert(ui_pixel_jump_offset(1) == -3);
    assert(ui_pixel_jump_offset(2) == -5);
    assert(ui_pixel_jump_offset(3) == -3);
    assert(ui_pixel_jump_offset(4) == 0);
    assert(ui_pixel_jump_offset(99) == 0);

    test_corner_cut_is_symmetric();
    test_corner_cut_pixel_count();
    test_corner_cut_degenerate_inputs();

    printf("test_ui_pixel_math: PASS\n");
    return 0;
}
