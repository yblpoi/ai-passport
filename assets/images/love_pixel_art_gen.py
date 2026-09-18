#!/usr/bin/env python3
"""纪念日摆件像素素材生成器（无第三方依赖）。

同一份素材同时产出两路结果，保证设备界面与后台网页的视觉完全一致：

1. `assets/images/love_pixel_art.c` + `main/love_pixel_art.h`
   —— 40x40 的 4bpp 索引图标（每张自带 16 色调色板，索引 0 恒为透明）与
   48x48 爱心底纹。
2. `assets/images/web/icon_<name>.png` + `assets/images/web/assets.json`
   —— 网页用的同款 PNG 与 base64 数据表（底纹、图标、调色板）。
3. `assets/images/web/contact-sheet.png` + `contact-sheet-zoom.png`
   —— 仅供人工核对的预览图（后者 3 倍放大、棋盘底色，看细节用）。

图标的素材是 `assets/images/emoji/` 下 16 张 Twemoji（CC-BY 4.0，来源与 sha256
见那里的 manifest.json，许可原文见 LICENSE-GRAPHICS.txt）：裁掉透明边、按长边
等比降到 20x20 逻辑像素（每格取 alpha 加权的主导色，得到硬边像素风），再整数倍
放大到 40x40。爱心底纹仍是本仓库手绘的 8x8 掩码。

用法（仓库根目录）：
    python3 assets/images/love_pixel_art_gen.py

底纹掩码字符：`.` 透明；其余见 PALETTE。每行必须恰好 8 个字符。
"""

from __future__ import annotations

import base64
import json
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEVICE_C = ROOT / "assets/images/love_pixel_art.c"
WEB_DIR = ROOT / "assets/images/web"

MASK_PX = 8      # 底纹掩码的边长（图标用的是下面的 Twemoji 素材）
EMOJI_SRC_DIR = ROOT / "assets/images/emoji"
EMOJI_PX = 20     # 降采样后的逻辑像素边长：40px 的屏上每格恰好 2x2
EMOJI_SCALE = 2   # 20 -> 40，整数倍放大才不会有半像素
ICON_PX = EMOJI_PX * EMOJI_SCALE
BG_TILE_PX = 48
ALPHA_MIN = 128           # 源像素参与裁边与投票的 alpha 门槛
COVER_NUM, COVER_DEN = 1, 2   # 一格算"不透明"的门槛：累计 alpha >= 半格
MAX_OPAQUE_COLORS = 15    # I4 的 16 个槽位里给透明留一格
ZOOM_SHEET_SCALE = 3      # 放大联络表：每图放大 3 倍（最近邻）
ZOOM_SHEET_COLUMNS = 8    # 一行放 8 个，16 张正好两行

# 为什么内置图标**不做**圆角:
#
# 它们是透明背景的**图形**,不是方块照片。实测 16 张生成图里只有猫咪的两个上角碰到
# 画布边缘 —— 4px 圆角只会切到它、其余 15 张毫无变化,这条规则就只会作用于唯一需要
# 注意边角的那个图标。图形类图标只有"保持原样"和"被切"两种选择,这里选保持原样;
# 圆形底座同理不做。
#
# 圆角只作用于**自定义头像**(照片,满幅方角),由 main/ui_pixel_math.c 的
# ui_pixel_corner_cut() 在 ui_pixel_pack_avatar_i4() 里把角落改指到透明索引;网页端
# assets/web/admin.css 用同一个半径(4px),并且只给自定义头像的 img 加圆角。


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

# 图标素材（Twemoji，见文件头）。顺序即 love_pixel_art.h 里生成的 LOVE_ICON_* 下标，
# 也是用户配置里存的图标号，绝不能改（改序 = 把用户选好的图标换掉）。
# 文件名是 Twemoji 的码位命名，来源与 sha256 见 assets/images/emoji/manifest.json。
EMOJI: list[tuple[str, str, str]] = [
    ("bird", "小鸟", "1f426"),
    ("cat", "猫咪", "1f431"),
    ("dog", "狗狗", "1f436"),
    ("rabbit", "兔子", "1f430"),
    ("bear", "小熊", "1f43b"),
    ("fox", "狐狸", "1f98a"),
    ("heart", "爱心", "2764"),
    ("star", "星星", "2b50"),
    ("flower", "小花", "1f338"),
    ("moon", "月饼", "1f96e"),
    ("cake", "蛋糕", "1f382"),
    ("gift", "礼物", "1f381"),
    ("balloon", "气球", "1f388"),
    ("ring", "戒指", "1f48d"),
    ("loving", "爱你", "1f970"),
    ("tree", "圣诞树", "1f384"),
    # 只能往后追加：下标 0..15 存在用户配置里，语义不能动。
    ("firecracker", "鞭炮", "1f9e8"),
    ("bouquet", "花束", "1f490"),
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
# 底纹只画爱心、不画底色：底色由设备 style 的 bg_color（网页端是 body 的
# background-color）提供。这样换底色只改一个颜色值，不用重新生成素材；
# 只有"白 + 透明"两色还能压成 I1，一张 296 字节（原来的 RGB565 是 4608 字节）。
BG_HEART = (0xFF, 0xFF, 0xFF)   # 白爱心
# 爱心按 30% 不透明度叠在底色上。100% 试过：纯白压粉底太抢眼，整屏像贴纸；
# 30% 只剩一层淡淡的光斑，底色是什么它就是什么色系（改底色不用动这里）。
# 索引图的调色板每项都是 (B,G,R,A)，alpha 直接参与混合 —— 设备端与网页端
# （PNG 的 alpha 通道）用的是同一个值，两端看起来才对得上。
BG_HEART_ALPHA = 77             # ≈ 255 x 0.30


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


# —— 以下是 Twemoji 图标路径的纯函数（接线见下一次提交，主机测试已经在用）——
#
# 设计要点，都是踩过的坑换来的：
#  * 只认「8 位索引色、无隔行」这一种 PNG。服务对象是我们自己 vendored 的 18 张素材，
#    别的格式一律抛错——将来上游换了导出参数时宁可炸掉，也不要悄悄解出错图。
#  * 降采样用「每格按 alpha 累加投票取主导色」，不是面积平均：平均会造出源图没有的
#    中间色（16 色索引图上就是脏色），也等于把像素风糊成低分辨率矢量。
#  * 所有取舍都走整数（不用 round()，它是银行家舍入），换台机器也能得到同一批字节。

def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def decode_indexed_png(data: bytes) -> tuple[int, int, list[tuple[int, int, int, int]], bytes]:
    """解一张调色板索引 PNG，返回 (宽, 高, 调色板[(R,G,B,A)], 索引字节)。

    tRNS 短于 PLTE 时，**余下的调色板项是完全不透明的**（PNG 规范如此，Twemoji 的
    18 张素材全都是短 tRNS）——这是这类解码最常见的坑，主机测试专门钉住它。
    """
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("不是 PNG 文件")
    pos = 8
    width = height = None
    plte = b""
    trns = b""
    idat = bytearray()
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        if len(payload) != length:
            raise ValueError(f"PNG 的 {tag!r} 块被截断")
        if tag == b"IHDR":
            width, height, depth, color_type, compression, filter_method, interlace = \
                struct.unpack(">IIBBBBB", payload)
            if (depth, color_type, interlace) != (8, 3, 0):
                raise ValueError(
                    f"只支持 8 位索引色无隔行 PNG，实际 depth={depth} "
                    f"colortype={color_type} interlace={interlace}")
            if compression != 0 or filter_method != 0:
                raise ValueError("PNG 的压缩/过滤方法不是 0")
        elif tag == b"PLTE":
            plte = payload
        elif tag == b"tRNS":
            trns = payload
        elif tag == b"IDAT":
            idat += payload
        elif tag == b"IEND":
            break
        pos += 12 + length

    if width is None or height is None:
        raise ValueError("PNG 缺 IHDR")
    if not plte or len(plte) % 3:
        raise ValueError("PNG 缺 PLTE 或长度不是 3 的倍数")
    palette = [(plte[i * 3], plte[i * 3 + 1], plte[i * 3 + 2],
                trns[i] if i < len(trns) else 255) for i in range(len(plte) // 3)]

    raw = zlib.decompress(bytes(idat))
    if len(raw) != height * (1 + width):
        raise ValueError("PNG 的 IDAT 长度与尺寸不符")

    out = bytearray()
    prev = bytearray(width)
    for y in range(height):
        offset = y * (1 + width)
        filter_type = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + width])
        if filter_type == 0:
            pass
        elif filter_type == 1:
            for i in range(1, width):
                line[i] = (line[i] + line[i - 1]) & 0xFF
        elif filter_type == 2:
            for i in range(width):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filter_type == 3:
            for i in range(width):
                left = line[i - 1] if i else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filter_type == 4:
            for i in range(width):
                left = line[i - 1] if i else 0
                up_left = prev[i - 1] if i else 0
                line[i] = (line[i] + _paeth(left, prev[i], up_left)) & 0xFF
        else:
            raise ValueError(f"未知的 PNG filter 类型 {filter_type}")
        out += line
        prev = line

    for value in out:
        if value >= len(palette):
            raise ValueError(f"索引 {value} 超出调色板（{len(palette)} 项）")
    return width, height, palette, bytes(out)


def emoji_bbox(width: int, height: int, palette, indices: bytes,
               alpha_min: int = ALPHA_MIN) -> tuple[int, int, int, int]:
    """不透明像素的外接框（闭区间），只算 alpha >= alpha_min 的像素。

    Twemoji 的图四边留着大小不一的透明边，裁掉它才能让每个图标在 20x20 的画布里
    尽量占满（月亮、气球这类形状尤其明显）。
    """
    x0, y0, x1, y1 = width, height, -1, -1
    for y in range(height):
        row = y * width
        for x in range(width):
            if palette[indices[row + x]][3] >= alpha_min:
                x0 = min(x0, x)
                x1 = max(x1, x)
                y0 = min(y0, y)
                y1 = max(y1, y)
    if x1 < 0:
        raise ValueError("这张图没有任何不透明像素")
    return x0, y0, x1, y1


def fit_box(box: tuple[int, int, int, int], n: int = EMOJI_PX):
    """把外接框**等比**缩进 n x n 并居中，返回 (x0,y0,w,h,w2,h2,ox,oy)。

    必须等比：18 张素材的外接框从 38x72（气球）到 72x72（蛋糕）都有，逐轴填满会把
    气球拉宽近一倍。代价是"墨迹占比"仍会差到 27%–81%（气球天生瘦），这是形状差异，
    不是可以靠拉伸抹平的东西。
    """
    x0, y0, x1, y1 = box
    w, h = x1 - x0 + 1, y1 - y0 + 1
    long_side = max(w, h)
    w2 = max(1, min(n, (w * n + long_side // 2) // long_side))
    h2 = max(1, min(n, (h * n + long_side // 2) // long_side))
    return x0, y0, w, h, w2, h2, (n - w2) // 2, (n - h2) // 2


def downsample_emoji(width: int, height: int, palette, indices: bytes,
                     box: tuple[int, int, int, int],
                     n: int = EMOJI_PX) -> list[list[tuple[int, int, int] | None]]:
    """裁边后降采样成 n x n 的逻辑像素，`None` 是透明格。

    每格对它覆盖的源像素按 alpha 累加投票，取票数最高的颜色；票数并列取 RGB 字典序
    较小的（全序、可复现，且偏向深色——眼睛和轮廓正是深色的那几票）。
    累计 alpha 不足半格的格子判为透明，透明背景素材的边角就是这样来的。
    """
    x0, y0, w, h, w2, h2, ox, oy = fit_box(box, n)
    grid: list[list[tuple[int, int, int] | None]] = [[None] * n for _ in range(n)]
    for j in range(h2):
        sy0 = y0 + j * h // h2
        sy1 = y0 + (j + 1) * h // h2
        if sy1 <= sy0:
            sy1 = sy0 + 1        # 源比目标小（放大）时才会走到这里
        for i in range(w2):
            sx0 = x0 + i * w // w2
            sx1 = x0 + (i + 1) * w // w2
            if sx1 <= sx0:
                sx1 = sx0 + 1
            votes: dict[tuple[int, int, int], int] = {}
            area = 0
            cover = 0
            for y in range(sy0, sy1):
                row = y * width
                for x in range(sx0, sx1):
                    r, g, b, a = palette[indices[row + x]]
                    area += 1
                    if a < ALPHA_MIN:
                        continue
                    key = (r, g, b)
                    votes[key] = votes.get(key, 0) + a
                    cover += a
            if cover * COVER_DEN < COVER_NUM * 255 * area:
                continue
            grid[oy + j][ox + i] = min(votes.items(), key=lambda kv: (-kv[1], kv[0]))[0]
    return grid


def load_emoji_grid(path: Path) -> list[list[tuple[int, int, int] | None]]:
    """读一张 Twemoji 素材，给出它 20x20 的主导色网格。"""
    width, height, palette, indices = decode_indexed_png(path.read_bytes())
    box = emoji_bbox(width, height, palette, indices)
    return downsample_emoji(width, height, palette, indices, box)


def per_icon_palette(grid) -> list[tuple[int, int, int, int]]:
    """每张图标一张 16 项调色板：索引 0 恒为透明，其余按「用得多的在前」排
    （并列取 RGB 小的）。用色超过 15 种直接抛错——I4 只有 16 个槽位，静默截断
    会让图标悄悄丢色。实测 18 张素材最多 10 色（月饼），余量充足。

    为什么每图一张表而不是全局一张：16 张的用色并集远超 16 种，而 lv_bin_decoder
    对索引格式的约定本就是「调色板在 image->data 开头」，天然支持每图自带。
    """
    counts: dict[tuple[int, int, int], int] = {}
    for row in grid:
        for cell in row:
            if cell is not None:
                counts[cell] = counts.get(cell, 0) + 1
    opaque = sorted(counts, key=lambda c: (-counts[c], c))
    if len(opaque) > MAX_OPAQUE_COLORS:
        raise ValueError(
            f"这张图降采样后用了 {len(opaque)} 种不透明色，I4 只放得下 "
            f"{MAX_OPAQUE_COLORS} 种（16 个槽位里要给透明留一格）")
    palette = [(0, 0, 0, 0)] + [(r, g, b, 255) for r, g, b in opaque]
    return palette + [(0, 0, 0, 0)] * (16 - len(palette))


def scaled_index_rows(grid, palette) -> list[list[int]]:
    """把逻辑像素网格放大 EMOJI_SCALE 倍、映射成这张图的调色板索引（透明格是 0）。

    设备那份 I4 数据与网页 PNG 都从这里出发 —— "放大 + 查索引"只写一份。
    """
    index = {px: i for i, px in enumerate(palette)}
    rows: list[list[int]] = []
    for row in grid:
        scaled: list[int] = []
        for cell in row:
            scaled.extend([0 if cell is None else
                           index[(cell[0], cell[1], cell[2], 255)]] * EMOJI_SCALE)
        rows.extend([scaled] * EMOJI_SCALE)
    return rows


def pack_icon_i4(grid, palette) -> bytes:
    """把逻辑像素网格打包成 LV_COLOR_FORMAT_I4 字节流：开头 16 项 (B,G,R,A)，
    后面每字节 2 像素、高半字节在前。整张 864 字节。"""
    packed = bytearray()
    for row in scaled_index_rows(grid, palette):
        for x in range(0, len(row), 2):
            packed.append((row[x] << 4) | row[x + 1])
    out = bytearray()
    for r, g, b, a in palette:
        out.extend((b, g, r, a))
    out.extend(packed)
    return bytes(out)


def grid_to_rows(grid, palette) -> list[list[tuple[int, int, int, int]]]:
    """把逻辑像素网格放大成 40x40 的 RGBA 行。

    网页 PNG 与放大联络表都用它，和设备那份 I4 数据出自同一个网格——两端不会画出
    不一样的东西。
    """
    return [[palette[i] for i in row] for row in scaled_index_rows(grid, palette)]


def build_zoom_sheet(arrays) -> list[list[tuple[int, int, int, int]]]:
    """人工验收用的放大联络表：每图放大 ZOOM_SHEET_SCALE 倍（最近邻）、棋盘格底色。

    40px 一行连排的 contact-sheet 看不出细节，透明边落在粉色底上尤其糊；棋盘底
    能一眼分辨"哪一格是透明、边界在哪"。
    """
    cell = ICON_PX * ZOOM_SHEET_SCALE
    sheet_rows = -(-len(arrays) // ZOOM_SHEET_COLUMNS)
    width = cell * ZOOM_SHEET_COLUMNS
    height = cell * sheet_rows
    tile = 8
    light, dark = (0xF2, 0xF2, 0xF2, 255), (0xD8, 0xD8, 0xD8, 255)
    sheet = [[light if (x // tile + y // tile) % 2 == 0 else dark for x in range(width)]
             for y in range(height)]
    for index, (_name, rows) in enumerate(arrays):
        ox = (index % ZOOM_SHEET_COLUMNS) * cell
        oy = (index // ZOOM_SHEET_COLUMNS) * cell
        for y, row in enumerate(rows):
            for x, px in enumerate(row):
                if px[3] == 0:
                    continue
                for dy in range(ZOOM_SHEET_SCALE):
                    line = sheet[oy + y * ZOOM_SHEET_SCALE + dy]
                    for dx in range(ZOOM_SHEET_SCALE):
                        line[ox + x * ZOOM_SHEET_SCALE + dx] = px
    return sheet


def build_bg_tile() -> list[list[tuple[int, int, int, int]]]:
    """48x48 无缝平铺:两颗错位白爱心,其余透明。

    返回值是 RGBA 行,透明处 alpha=0 —— 网页那张 bg_tile.png 直接用它写出,
    设备端再把它压成 I1(见 generate 里的 bg_palette)。底色两边都不在这张图里。
    """
    tile = [[(0, 0, 0, 0) for _ in range(BG_TILE_PX)] for _ in range(BG_TILE_PX)]
    heart = scale(parse_mask(HEART_MASK), 2)  # 16x16
    for origin_y, origin_x in ((6, 6), (30, 30)):
        for r, row in enumerate(heart):
            for c, (_rr, _gg, _bb, alpha) in enumerate(row):
                if alpha == 0:
                    continue
                y = origin_y + r
                x = origin_x + c
                if 0 <= y < BG_TILE_PX and 0 <= x < BG_TILE_PX:
                    tile[y][x] = (*BG_HEART, BG_HEART_ALPHA)
    return tile


def generate(root: Path = ROOT) -> int:
    """生成全部素材，返回图标张数。

    `root` 只有主机测试会传：它把整条流水线跑进临时目录再对产物做断言，
    所以这里不直接写死模块级的那几个路径常量。
    """
    device_c = root / "assets/images/love_pixel_art.c"
    device_h = root / "main/love_pixel_art.h"
    web_dir = root / "assets/images/web"
    device_c.parent.mkdir(parents=True, exist_ok=True)
    device_h.parent.mkdir(parents=True, exist_ok=True)

    icons: list[tuple[str, str]] = []
    arrays: list[tuple[str, list[list[tuple[int, int, int, int]]]]] = []
    grids: list[list[list[tuple[int, int, int] | None]]] = []
    palettes: list[list[tuple[int, int, int, int]]] = []

    for name, label, code in EMOJI:
        grid = load_emoji_grid(EMOJI_SRC_DIR / f"{code}.png")
        palette = per_icon_palette(grid)
        grids.append(grid)
        palettes.append(palette)
        arrays.append((name, grid_to_rows(grid, palette)))
        icons.append((name, label))

    c_lines = [
        "// assets/images/love_pixel_art.c —— 由 assets/images/love_pixel_art_gen.py 生成,请勿手改。",
        "// 同一批网格也导出到 assets/images/web/ 供后台网页使用,两端视觉一致。",
        "//",
        "// 图标是 LV_COLOR_FORMAT_I4:数据开头是 16 个 lv_color32_t 调色板(内存顺序",
        "// B,G,R,A),后面是每字节 2 像素、高半字节在前的索引。这是 lv_bin_decoder 对",
        "// LV_IMAGE_SRC_VARIABLE + 索引格式的约定(见 decode_indexed:palette 取",
        "// image->data 开头,索引数据从 image->data + palette_size*4 开始)。",
        '#include "love_pixel_art.h"',
        "",
    ]

    row_bytes = ICON_PX // 2

    for (name, _rows), grid, palette in zip(arrays, grids, palettes):
        # 打包走主机测试覆盖的纯函数：数据开头 16*4 字节是这张图自己的调色板。
        data = pack_icon_i4(grid, palette)
        packed = data[16 * 4:]

        c_lines.append(
            f"static const uint8_t icon_{name}_data[{16 * 4 + ICON_PX * row_bytes}] = {{")
        for i in range(16):
            b, g, r, a = data[i * 4:i * 4 + 4]
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

    # 底纹只有"透明 + 半透明白"两色,用 I1 就够:每字节 8 像素、高位在前,数据开头是
    # 2 项 (B,G,R,A) 调色板 —— 与图标同为 lv_bin_decoder 对索引格式的约定,
    # 见 decode_indexed_line。整张 8 + 288 = 296 字节,原 RGB565 是 4608。
    bg_palette = [(0, 0, 0, 0), (*BG_HEART, BG_HEART_ALPHA)]
    bg_bits = bytearray()
    for row in tile:
        for x in range(0, BG_TILE_PX, 8):
            byte = 0
            for bit in range(8):
                if row[x + bit][3] != 0:
                    byte |= 0x80 >> bit
            bg_bits.append(byte)

    bg_row_bytes = BG_TILE_PX // 8
    c_lines.append(
        f"static const uint8_t bg_tile_data[{len(bg_palette) * 4 + len(bg_bits)}] = {{")
    for i, (r, g, b, a) in enumerate(bg_palette):
        c_lines.append(f"    /* pal{i} */ 0x{b:02X}, 0x{g:02X}, 0x{r:02X}, 0x{a:02X},")
    for off in range(0, len(bg_bits), bg_row_bytes):
        c_lines.append("    " + ", ".join(
            f"0x{v:02X}" for v in bg_bits[off:off + bg_row_bytes]) + ",")
    c_lines += [
        "};",
        "",
        "static const lv_image_dsc_t bg_tile = {",
        "    .header = { .cf = LV_COLOR_FORMAT_I1,",
        f"                .w = {BG_TILE_PX}, .h = {BG_TILE_PX}, .stride = {BG_TILE_PX} / 8 }},",
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
    device_c.write_text("\n".join(c_lines), encoding="utf-8")

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
    device_h.write_text("\n".join(header), encoding="utf-8")

    # 调色板定义写进 .c,和图标数据放一起。
    c_lines += [
        "const uint32_t love_pixel_palette[LOVE_PALETTE_COUNT] = {",
    ]
    for ch in PALETTE_ORDER:
        r, g, b = PALETTE[ch]
        c_lines.append(f"    0x{r:02X}{g:02X}{b:02X},  // {ch}")
    c_lines += ["};", ""]
    device_c.write_text("\n".join(c_lines), encoding="utf-8")

    web_dir.mkdir(parents=True, exist_ok=True)
    web_entries = []
    for name, rows in arrays:
        png_path = web_dir / f"icon_{name}.png"
        write_png(png_path, rows)
        web_entries.append({
            "id": name,
            "label": dict(icons)[name],
            "data": "data:image/png;base64,"
                    + base64.b64encode(png_path.read_bytes()).decode("ascii"),
        })

    # 爱心底纹同样导出给网页,保证后台和设备是同一张壁纸(同样只画爱心不画底色,
    # 网页端由 body 与 .screen 的 background-color 提供粉底)。
    bg_png = web_dir / "bg_tile.png"
    write_png(bg_png, tile)
    bg_data_uri = ("data:image/png;base64,"
                   + base64.b64encode(bg_png.read_bytes()).decode("ascii"))

    palette_hex = ["#%02X%02X%02X" % PALETTE[ch] for ch in PALETTE_ORDER]
    # 只留 assets.json：网页唯一消费的素材表就是它（内联进 admin.js 的是它的
    # icons 与 palette），再写一份内容相同的 icons.json 只会多一个没人读的孤儿。
    (web_dir / "assets.json").write_text(
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
    write_png(web_dir / "contact-sheet.png", sheet)
    write_png(web_dir / "contact-sheet-zoom.png", build_zoom_sheet(arrays))

    return len(arrays)


def main() -> None:
    count = generate()
    print(f"icons: {count}")
    print(f"device: {DEVICE_C.relative_to(ROOT)} {DEVICE_C.stat().st_size // 1024} KB")
    print(f"web:    {WEB_DIR.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
