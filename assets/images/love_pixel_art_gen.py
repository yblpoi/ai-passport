#!/usr/bin/env python3
"""纪念日摆件像素素材生成器（无第三方依赖）。

同一份 8x8 像素掩码同时产出两路结果，保证设备界面与后台网页的视觉完全一致：

1. `assets/images/love_pixel_art.c` + `main/love_pixel_art.h`
   —— 40x40 的 4bpp 索引图标（掩码放大 5 倍，整数倍放缩才不会有半像素；调色板
   16 色内嵌在数据头部）与 48x48 爱心底纹。
2. `assets/images/web/icon_<name>.png` + `assets/images/web/icons.json`
   —— 网页用的同款 PNG 与 base64 数据表。
3. `assets/images/web/contact-sheet.png` —— 仅供人工核对的预览图。

用法（仓库根目录）：
    python3 assets/images/love_pixel_art_gen.py

掩码字符：`.` 透明；其余见 PALETTE。每行必须恰好 8 个字符。
"""

from __future__ import annotations

import base64
import json
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEVICE_C = ROOT / "assets/images/love_pixel_art.c"
DEVICE_H = ROOT / "main/love_pixel_art.h"
WEB_DIR = ROOT / "assets/images/web"

MASK_PX = 8      # 掩码边长
ICON_SCALE = 5   # 8 -> 40，整数倍放大才不会有半像素
ICON_PX = MASK_PX * ICON_SCALE
BG_TILE_PX = 48

# 为什么内置图标**不做**圆角(以及为什么"缩小一点"解决不了问题):
#
# 这些图标是 8x8 掩码放大 5 倍的**透明背景图形**,不是方块牌。四角有 8 个图标的描边
# 确实伸到了画布边缘(猫/狗/熊/狐狸的顶部两角、星星/蛋糕/礼物的底部两角、叶子各一个),
# 施半径 4px 的圆角会各切掉 6 个描边像素,合计 48 个(占全部图标像素的 0.19%)。
# 而"把图标缩小一点让圆角只落在空白上"是无效的:缩小后圆角落在透明上,视觉上等于
# 没有圆角,唯一可见的变化就是图标变小了 —— 8x8 掩码从 5x 降到 4x 会小 20%。
# 换句话说图形类图标要么保持原样(方角,但没人看得出"方"),要么被切;两者之间没有
# 更好的第三选项。这里选保持原样。
#
# 圆角只作用于**自定义头像**(照片,满幅方角),由 main/ui_pixel_math.c 的
# ui_pixel_corner_cut() 在 ui_pixel_pack_avatar_i4() 里把角落改指到透明索引;网页端 admin.css
# 用同一个半径,并且只给自定义头像的 img 加圆角(给图形图标加会同样切到描边)。
# 圆形底座仍然不做:实测圆形会让 16 个角色里 15 个掉实心像素(猫咪少 186、礼物少 284)。


# 与 main/ui_pixel.h 的配色保持一致，避免设备与网页出现两套颜色。
PALETTE = {
    "K": (0x17, 0x20, 0x2A),  # 墨色描边
    "W": (0xFF, 0xFF, 0xFF),  # 白
    "w": (0xF4, 0xF4, 0xEA),  # 纸白
    "R": (0xE4, 0x3B, 0x2F),  # 红
    "r": (0xF1, 0x93, 0x9C),  # 粉
    "L": (0xF7, 0xBF, 0xC4),  # 浅粉
    "D": (0x8A, 0x5A, 0x33),  # 棕
    "S": (0xF7, 0xD9, 0xB8),  # 肤色
    "O": (0xFF, 0xB2, 0x3E),  # 橙
    "Y": (0xFF, 0xD9, 0x28),  # 黄
    "G": (0x82, 0xBE, 0x2D),  # 绿
    "g": (0x55, 0x95, 0x1D),  # 深绿
    "B": (0x16, 0x89, 0xE8),  # 蓝
    "b": (0xB9, 0xF3, 0xFF),  # 浅蓝
    "P": (0x75, 0x57, 0xD9),  # 紫
    "e": (0x9E, 0x9E, 0x9E),  # 浅灰
}

# 调色板顺序即自定义头像 4bpp 的索引顺序,改动等于让所有已上传头像换色,
# 所以必须显式写死而不是依赖 dict 的插入顺序。
PALETTE_ORDER = ["K", "W", "w", "R", "r", "L", "D", "S", "O", "Y", "G", "g", "B", "b", "P", "e"]
assert len(PALETTE_ORDER) == 16, "4bpp 自定义头像需要恰好 16 个索引色"
assert set(PALETTE_ORDER) == set(PALETTE), "PALETTE_ORDER 与 PALETTE 必须一一对应"

# 顺序必须与 love_pixel_art.h 中生成的 LOVE_ICON_* 常量一致。
ICONS: list[tuple[str, str, list[str]]] = [
    ("bird", "小鸟", [
        "........",
        "..KKK...",
        ".KYYYK..",
        ".KYKYYKO",
        ".KYYYYKO",
        ".KYYYYK.",
        "..KYYK..",
        "...KK...",
    ]),
    ("cat", "猫咪", [
        "KK....KK",
        "KeeKKeeK",
        "KeeeeeeK",
        "KeKeeKeK",
        "KeeeeeeK",
        "KewKKweK",
        ".KeeeeK.",
        "..KKKK..",
    ]),
    ("dog", "狗狗", [
        "K......K",
        "KKDDDDKK",
        "KDDDDDDK",
        "KDKDDKDK",
        "KDDDDDDK",
        "KDSSSSDK",
        ".KDSSDK.",
        "..KKKK..",
    ]),
    ("rabbit", "兔子", [
        ".KK..KK.",
        ".KwKKwK.",
        ".KwwwwK.",
        ".KwKKwK.",
        ".KwwwwK.",
        "..KwwK..",
        "..KKKK..",
        "........",
    ]),
    ("bear", "小熊", [
        "KK....KK",
        "KDDKKDDK",
        "KDDDDDDK",
        "KDKDDKDK",
        "KDDDDDDK",
        "KDDSSDDK",
        ".KDDDDK.",
        "..KKKK..",
    ]),
    ("fox", "狐狸", [
        "K......K",
        "KOK..KOK",
        "KOOOOOOK",
        "KOKOOKOK",
        "KOWWWWOK",
        "KOWWWWOK",
        ".KOWWOK.",
        "..KKKK..",
    ]),
    ("heart", "爱心", [
        "........",
        ".KK..KK.",
        "KRRKKRRK",
        "KRRRRRRK",
        "KRRRRRRK",
        ".KRRRRK.",
        "..KRRK..",
        "...KK...",
    ]),
    ("star", "星星", [
        "...KK...",
        "..KYYK..",
        "KKKYYKKK",
        "KYYYYYYK",
        ".KYYYYK.",
        ".KYYYYK.",
        "KYK..KYK",
        "KK....KK",
    ]),
    ("flower", "小花", [
        "..KrrK..",
        ".KrrrrK.",
        "KrYYYYrK",
        "KrYYYYrK",
        ".KrrrrK.",
        ".KgKggK.",
        "..KggK..",
        "...KK...",
    ]),
    ("moon", "月亮", [
        "...KKKK.",
        "..KYYYYK",
        ".KYYYYK.",
        ".KYYYK..",
        ".KYYYK..",
        ".KYYYYK.",
        "..KYYYYK",
        "...KKKK.",
    ]),
    ("cake", "蛋糕", [
        "...O....",
        "...K....",
        "..KKKK..",
        ".KWWWWK.",
        ".KwWwWK.",
        ".KKKKKK.",
        ".KrrrrK.",
        "KKKKKKKK",
    ]),
    ("gift", "礼物", [
        ".KK..KK.",
        "KRRKKRRK",
        "KKKKKKKK",
        "KYYRRYYK",
        "KYYRRYYK",
        "KYYRRYYK",
        "KYYRRYYK",
        "KKKKKKKK",
    ]),
    ("balloon", "气球", [
        "..KKKK..",
        ".KRRRRK.",
        "KRRRRRRK",
        "KRRRRRRK",
        "KRRRRRRK",
        ".KRRRRK.",
        "..KKKK..",
        "...K....",
    ]),
    ("ring", "戒指", [
        "...KK...",
        "..KbbK..",
        "..KbbK..",
        ".KKKKKK.",
        "KYYYYYYK",
        "KYK..KYK",
        ".KYYYYK.",
        "..KKKK..",
    ]),
    ("leaf", "叶子", [
        ".....KKK",
        "...KKGGK",
        ".KKGGGK.",
        "KGGGGK..",
        "KGGGK...",
        "KGGK....",
        "KGK.....",
        "KK......",
    ]),
    ("tree", "圣诞树", [
        "...Y....",
        "..KYK...",
        ".KGGGK..",
        "KGGRGGK.",
        "KKKKKKK.",
        "KGGGGGK.",
        "KKKKKKK.",
        "..KDDK..",
    ]),
]

# 主屏底纹用的爱心（8x8 掩码，放大 2 倍后放进 48x48 平铺砖）。
HEART_MASK = [
    ".KK..KK.",
    "KrLKKLrK",
    "KrLLLLrK",
    "KrLLLLrK",
    ".KrLLrK.",
    "..KrLrK.",
    "...KrK..",
    "....K...",
]
BG_BASE = (0xF1, 0x95, 0x9E)    # 粉色底
BG_HEART = (0xEA, 0xA5, 0xAC)   # 略深的粉爱心


def parse_mask(mask: list[str], size: int = MASK_PX) -> list[list[tuple[int, int, int, int]]]:
    assert len(mask) == size, f"掩码需要 {size} 行，实际 {len(mask)}"
    rows = []
    for row in mask:
        assert len(row) == size, f"掩码行宽需要 {size}，实际 {len(row)}: {row!r}"
        pixels = []
        for ch in row:
            if ch == ".":
                pixels.append((0, 0, 0, 0))
            else:
                assert ch in PALETTE, f"未知色号 {ch!r}"
                r, g, b = PALETTE[ch]
                pixels.append((r, g, b, 255))
        rows.append(pixels)
    return rows


def scale(rows, factor: int):
    out = []
    for row in rows:
        big = []
        for px in row:
            big.extend([px] * factor)
        for _ in range(factor):
            out.append(list(big))
    return out


def write_png(path: Path, rows: list[list[tuple[int, int, int, int]]]) -> None:
    """手写最小 PNG 编码器，避免引入 Pillow 依赖。"""
    height = len(rows)
    width = len(rows[0])
    raw = bytearray()
    for row in rows:
        raw.append(0)  # filter type 0
        for r, g, b, a in row:
            raw.extend((r, g, b, a))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    path.write_bytes(png)


def build_bg_tile() -> list[list[tuple[int, int, int]]]:
    """48x48 无缝平铺:两颗错位爱心，对应截图里的爱心壁纸。"""
    base = BG_BASE
    tile = [[base for _ in range(BG_TILE_PX)] for _ in range(BG_TILE_PX)]
    heart = scale(parse_mask(HEART_MASK), 2)  # 16x16
    for origin_y, origin_x in ((6, 6), (30, 30)):
        for r, row in enumerate(heart):
            for c, (_rr, _gg, _bb, alpha) in enumerate(row):
                if alpha == 0:
                    continue
                y = origin_y + r
                x = origin_x + c
                if 0 <= y < BG_TILE_PX and 0 <= x < BG_TILE_PX:
                    tile[y][x] = BG_HEART
    return tile


def build_icon_palette(arrays) -> list[tuple[int, int, int, int]]:
    """图标的 I4 调色板：索引 0 固定为透明（图标外轮廓内部是镂空的），
    其余按 PALETTE_ORDER 顺序排列图标真正用到的颜色。

    注意与 love_pixel_palette 的区别：那张 16 色表是自定义头像的索引顺序，
    没有透明项（网页上传的头像恒为不透明）。图标要透明，所以自带一张表——
    lv_bin_decoder 对 LV_IMAGE_SRC_VARIABLE + 索引格式的约定就是
    palette 位于 image->data 开头，因此每张图都能带自己的 16 色。"""
    used = set()
    for _name, rows in arrays:
        for row in rows:
            used.update(row)

    transparent = (0, 0, 0, 0)
    palette = [transparent]
    for ch in PALETTE_ORDER:
        r, g, b = PALETTE[ch]
        if (r, g, b, 255) in used:
            palette.append((r, g, b, 255))

    opaque_used = used - {transparent}
    assert len(opaque_used) + 1 <= 16, (
        f"I4 只有 16 个槽位：图标用了 {len(opaque_used)} 种不透明色 + 透明，放不下")
    assert len(palette) - 1 == len(opaque_used), (
        "有图标颜色没登记在 PALETTE/PALETTE_ORDER 里，会在生成时被悄悄丢掉")

    return palette + [(0, 0, 0, 0)] * (16 - len(palette))


def main() -> None:
    icons: list[tuple[str, str]] = []
    arrays: list[tuple[str, list[list[tuple[int, int, int, int]]]]] = []

    for name, label, mask in ICONS:
        arrays.append((name, scale(parse_mask(mask), ICON_SCALE)))
        icons.append((name, label))

    c_lines = [
        "// assets/images/love_pixel_art.c —— 由 assets/images/love_pixel_art_gen.py 生成,请勿手改。",
        "// 同一份掩码也导出到 assets/images/web/ 供后台网页使用,两端视觉一致。",
        "//",
        "// 图标是 LV_COLOR_FORMAT_I4:数据开头是 16 个 lv_color32_t 调色板(内存顺序",
        "// B,G,R,A),后面是每字节 2 像素、高半字节在前的索引。这是 lv_bin_decoder 对",
        "// LV_IMAGE_SRC_VARIABLE + 索引格式的约定(见 decode_indexed:palette 取",
        "// image->data 开头,索引数据从 image->data + palette_size*4 开始)。",
        '#include "love_pixel_art.h"',
        "",
    ]

    icon_palette = build_icon_palette(arrays)
    palette_index = {px: i for i, px in enumerate(icon_palette)}
    row_bytes = ICON_PX // 2

    for name, rows in arrays:
        packed = bytearray()
        for row in rows:
            for x in range(0, ICON_PX, 2):
                packed.append((palette_index[row[x]] << 4) | palette_index[row[x + 1]])

        c_lines.append(
            f"static const uint8_t icon_{name}_data[{16 * 4 + ICON_PX * row_bytes}] = {{")
        for i, (r, g, b, a) in enumerate(icon_palette):
            c_lines.append(f"    /* pal{i:2d} */ 0x{b:02X}, 0x{g:02X}, 0x{r:02X}, 0x{a:02X},")
        for off in range(0, len(packed), row_bytes):
            c_lines.append("    " + ", ".join(
                f"0x{v:02X}" for v in packed[off:off + row_bytes]) + ",")
        c_lines += [
            "};",
            "",
            f"static const lv_image_dsc_t icon_{name} = {{",
            "    .header = { .cf = LV_COLOR_FORMAT_I4,",
            f"                .w = {ICON_PX}, .h = {ICON_PX}, .stride = {ICON_PX} / 2 }},",
            f"    .data_size = sizeof(icon_{name}_data),",
            f"    .data = (const uint8_t *)icon_{name}_data,",
            "};",
            "",
        ]

    tile = build_bg_tile()

    def rgb565(px: tuple[int, int, int]) -> int:
        r, g, b = px
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

    c_lines.append(f"static const uint16_t bg_tile_data[{BG_TILE_PX * BG_TILE_PX}] = {{")
    for row in tile:
        c_lines.append("    " + ", ".join(f"0x{rgb565(px):04X}" for px in row) + ",")
    c_lines += [
        "};",
        "",
        "static const lv_image_dsc_t bg_tile = {",
        "    .header = { .cf = LV_COLOR_FORMAT_RGB565,",
        f"                .w = {BG_TILE_PX}, .h = {BG_TILE_PX}, .stride = {BG_TILE_PX} * 2 }},",
        "    .data_size = sizeof(bg_tile_data),",
        "    .data = (const uint8_t *)bg_tile_data,",
        "};",
        "",
        f"static const lv_image_dsc_t *const ICONS[{len(arrays)}] = {{",
        *[f"    &icon_{name}," for name, _ in arrays],
        "};",
        "",
        "const lv_image_dsc_t *love_pixel_icon(uint8_t index)",
        "{",
        "    if (index >= sizeof(ICONS) / sizeof(ICONS[0])) return ICONS[0];",
        "    return ICONS[index];",
        "}",
        "",
        "const lv_image_dsc_t *love_pixel_bg_tile(void)",
        "{",
        "    return &bg_tile;",
        "}",
        "",
    ]
    DEVICE_C.write_text("\n".join(c_lines), encoding="utf-8")

    header = [
        "// main/love_pixel_art.h —— 由 assets/images/love_pixel_art_gen.py 生成,请勿手改。",
        "// 像素图标与爱心底纹:设备界面与后台网页共用同一份素材。",
        "#pragma once",
        "",
        '#include "lvgl.h"',
        "",
        "// 图标序号,与后台网页的图标选择顺序一致。",
    ]
    for idx, (name, label) in enumerate(icons):
        header.append(f"#define LOVE_ICON_{name.upper()} {idx}  // {label}")
    header += [
        "",
        f"#define LOVE_ICON_COUNT {len(icons)}",
        f"#define LOVE_ICON_PX {ICON_PX}",
        f"#define LOVE_BG_TILE_PX {BG_TILE_PX}",
        "",
        "// 16 色调色板,顺序即自定义头像的 4bpp 索引顺序。",
        "// 后台网页按同一张表量化上传的图片,所以两端颜色是同一套。",
        f"#define LOVE_PALETTE_COUNT {len(PALETTE_ORDER)}",
        "extern const uint32_t love_pixel_palette[LOVE_PALETTE_COUNT];",
        "",
        "// 越界时返回第 0 个图标,不返回 NULL,调用方无需判空。",
        "const lv_image_dsc_t *love_pixel_icon(uint8_t index);",
        "const lv_image_dsc_t *love_pixel_bg_tile(void);",
        "",
    ]
    DEVICE_H.write_text("\n".join(header), encoding="utf-8")

    # 调色板定义写进 .c,和图标数据放一起。
    c_lines += [
        "const uint32_t love_pixel_palette[LOVE_PALETTE_COUNT] = {",
    ]
    for ch in PALETTE_ORDER:
        r, g, b = PALETTE[ch]
        c_lines.append(f"    0x{r:02X}{g:02X}{b:02X},  // {ch}")
    c_lines += ["};", ""]
    DEVICE_C.write_text("\n".join(c_lines), encoding="utf-8")

    WEB_DIR.mkdir(parents=True, exist_ok=True)
    web_entries = []
    for name, rows in arrays:
        png_path = WEB_DIR / f"icon_{name}.png"
        write_png(png_path, rows)
        web_entries.append({
            "id": name,
            "label": dict(icons)[name],
            "data": "data:image/png;base64,"
                    + base64.b64encode(png_path.read_bytes()).decode("ascii"),
        })

    # 爱心底纹同样导出给网页,保证后台和设备是同一张壁纸。
    bg_png = WEB_DIR / "bg_tile.png"
    write_png(bg_png, [[(r, g, b, 255) for (r, g, b) in row] for row in tile])
    bg_data_uri = ("data:image/png;base64,"
                   + base64.b64encode(bg_png.read_bytes()).decode("ascii"))

    palette_hex = ["#%02X%02X%02X" % PALETTE[ch] for ch in PALETTE_ORDER]
    (WEB_DIR / "icons.json").write_text(
        json.dumps(web_entries, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (WEB_DIR / "assets.json").write_text(
        json.dumps({ "bgTile": bg_data_uri, "icons": web_entries, "palette": palette_hex },
                   ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    # 人工核对用的联络表:一行放完所有图标。
    sheet_w = ICON_PX * len(arrays)
    sheet = [[(0xF1, 0x95, 0x9E, 255) for _ in range(sheet_w)] for _ in range(ICON_PX)]
    for idx, (_name, rows) in enumerate(arrays):
        for r, row in enumerate(rows):
            for c, px in enumerate(row):
                if px[3] == 0:
                    continue
                sheet[r][c + idx * ICON_PX] = (px[0], px[1], px[2], 255)
    write_png(WEB_DIR / "contact-sheet.png", sheet)

    print(f"icons: {len(arrays)}")
    print(f"device: {DEVICE_C.relative_to(ROOT)} {DEVICE_C.stat().st_size // 1024} KB")
    print(f"web:    {WEB_DIR.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
