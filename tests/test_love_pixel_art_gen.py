#!/usr/bin/env python3
"""Host tests for the commemorative-day icon generator.

生成器把 Twemoji 素材降采样成 20x20 的主导色网格、再打包成 LVGL 的 I4 图标。
这里钉住三件事：

1. 调色板索引 PNG 的解码（短 tRNS、五种 filter、畸形输入必须抛错）；
2. 降采样的取舍（裁边、等比居中、投票、并列与透明边界）——它们决定图标长什么样；
3. **不变量**：图标顺序、每张 864 字节，以及那张给自定义头像用的 16 色调色板
   一字不许改（改一个字，所有已上传头像就会换色）。
"""

from __future__ import annotations

import importlib.util
import json
import re
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "love_pixel_art_gen", ROOT / "assets" / "images" / "love_pixel_art_gen.py")
assert SPEC and SPEC.loader
GEN = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GEN
SPEC.loader.exec_module(GEN)

# 16 张素材裁边、等比降到 20 逻辑像素后的实测结果（长边恒 20、居中）。
# 气球 11x20、圣诞树 15x20 是形状本来的样子——**不许改成逐轴填满**（那是拉伸）。
FIT_EXPECTED = {
    "bird": (20, 18, 0, 1),
    "cat": (20, 20, 0, 0),
    "dog": (20, 19, 0, 0),
    "rabbit": (18, 20, 1, 0),
    "bear": (19, 20, 0, 0),
    "fox": (20, 18, 0, 1),
    "heart": (20, 18, 0, 1),
    "star": (20, 19, 0, 0),
    "flower": (20, 19, 0, 0),
    "moon": (20, 17, 0, 1),
    "cake": (20, 20, 0, 0),
    "gift": (20, 19, 0, 0),
    "balloon": (11, 20, 4, 0),
    "ring": (13, 20, 3, 0),
    "loving": (20, 20, 0, 0),
    "tree": (15, 20, 2, 0),
}

# 自定义头像的 4bpp 索引色序：存在 NVS 里的头像数据就是按这个顺序索引的，
# 动它 = 让用户已上传的头像全部换色。
EXPECTED_PALETTE_ORDER = ["K", "W", "w", "R", "r", "L", "D", "S", "O", "Y", "G", "g", "B", "b", "P", "e"]
EXPECTED_PALETTE = {
    "K": (0x17, 0x20, 0x2A),
    "W": (0xFF, 0xFF, 0xFF),
    "w": (0xF4, 0xF4, 0xEA),
    "R": (0xE4, 0x3B, 0x2F),
    "r": (0xF1, 0x93, 0x9C),
    "L": (0xF7, 0xBF, 0xC4),
    "D": (0x8A, 0x5A, 0x33),
    "S": (0xF7, 0xD9, 0xB8),
    "O": (0xFF, 0xB2, 0x3E),
    "Y": (0xFF, 0xD9, 0x28),
    "G": (0x82, 0xBE, 0x2D),
    "g": (0x55, 0x95, 0x1D),
    "B": (0x16, 0x89, 0xE8),
    "b": (0xB9, 0xF3, 0xFF),
    "P": (0x75, 0x57, 0xD9),
    "e": (0x9E, 0x9E, 0x9E),
}

RED = (255, 0, 0, 255)
BLUE = (0, 0, 255, 255)
CLEAR = (0, 0, 0, 0)


def png_chunk(tag: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + tag + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))


def encode_indexed_png(width, height, palette, rows, filters=None, *,
                       color_type=3, bitdepth=8, interlace=0, idat_parts=1,
                       trns_full=False):
    """把索引图编成 PNG（测试用，能指定每行 filter 与各种畸形参数）。

    `trns_full=False` 时按 PNG 规范写**短的** tRNS（只到最后一个非 255 项）。
    """
    plte = b"".join(bytes(px[:3]) for px in palette)
    if trns_full:
        trns = bytes(px[3] for px in palette)
    else:
        last = 0
        for i, px in enumerate(palette):
            if px[3] != 255:
                last = i + 1
        trns = bytes(px[3] for px in palette[:last])

    raw = bytearray()
    prev = [0] * width
    for y, line in enumerate(rows):
        kind = 0 if filters is None else filters[y]
        if kind == 0:
            filtered = list(line)
        elif kind == 1:
            filtered = [(line[i] - (line[i - 1] if i else 0)) & 0xFF for i in range(width)]
        elif kind == 2:
            filtered = [(line[i] - prev[i]) & 0xFF for i in range(width)]
        elif kind == 3:
            filtered = [(line[i] - (((line[i - 1] if i else 0) + prev[i]) >> 1)) & 0xFF
                        for i in range(width)]
        elif kind == 4:
            filtered = [(line[i] - GEN._paeth(line[i - 1] if i else 0, prev[i],
                                              prev[i - 1] if i else 0)) & 0xFF
                        for i in range(width)]
        else:
            raise AssertionError(f"测试自己不认识的 filter {kind}")
        raw += bytes([kind]) + bytes(filtered)
        prev = line

    out = b"\x89PNG\r\n\x1a\n"
    out += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, bitdepth,
                                          color_type, 0, 0, interlace))
    out += png_chunk(b"PLTE", plte)
    if trns:
        out += png_chunk(b"tRNS", trns)
    data = zlib.compress(bytes(raw), 9)
    step = max(1, -(-len(data) // idat_parts))
    for i in range(0, len(data), step):
        out += png_chunk(b"IDAT", data[i:i + step])
    out += png_chunk(b"IEND", b"")
    return out


def real_asset(code: str) -> tuple[int, int, list, bytes]:
    return GEN.decode_indexed_png((GEN.EMOJI_SRC_DIR / f"{code}.png").read_bytes())


def device_palette_hex() -> list[str]:
    """从已生成的 .c 里取 love_pixel_palette 的 16 个 0xRRGGBB。"""
    text = (ROOT / "assets" / "images" / "love_pixel_art.c").read_text(encoding="utf-8")
    block = text.split("const uint32_t love_pixel_palette[LOVE_PALETTE_COUNT] = {", 1)[1]
    return re.findall(r"0x([0-9A-F]{6})", block.split("};", 1)[0])


class DecodeIndexedPngTest(unittest.TestCase):
    def test_every_filter_type_round_trips(self):
        """五种 filter 都要能还原——Twemoji 目前只用 0，但上游换导出参数时不能静默解错。"""
        width, height = 4, 3
        palette = [(10, 20, 30, 255), (200, 100, 50, 255), (0, 0, 0, 0)]
        rows = [[0, 1, 2, 0], [1, 1, 0, 2], [2, 0, 1, 1]]
        for kind in range(5):
            data = encode_indexed_png(width, height, palette, rows, filters=[kind] * height)
            w, h, pal, indices = GEN.decode_indexed_png(data)
            self.assertEqual((w, h), (width, height))
            self.assertEqual(pal, palette)
            self.assertEqual(list(indices), [v for row in rows for v in row])

    def test_trns_shorter_than_plte_leaves_the_rest_opaque(self):
        """tRNS 短于 PLTE 时余项是完全不透明的（16 张素材全是短 tRNS）。"""
        palette = [(1, 2, 3, 255), (4, 5, 6, 0), (7, 8, 9, 255)]
        data = encode_indexed_png(2, 1, palette, [[0, 1]])
        _w, _h, pal, _idx = GEN.decode_indexed_png(data)
        self.assertEqual(pal[1][3], 0)
        self.assertEqual(pal[2][3], 255, "tRNS 之后的项必须补成 255，否则大片误判成透明")

    def test_long_trns_is_also_accepted(self):
        palette = [(1, 2, 3, 128), (4, 5, 6, 200)]
        data = encode_indexed_png(2, 1, palette, [[0, 1]], trns_full=True)
        _w, _h, pal, _idx = GEN.decode_indexed_png(data)
        self.assertEqual([px[3] for px in pal], [128, 200])

    def test_rejects_formats_other_than_8bit_indexed(self):
        palette = [(1, 2, 3, 255)]
        good = dict(width=1, height=1, palette=palette, rows=[[0]])
        for kwargs in ({"color_type": 6}, {"bitdepth": 16}, {"interlace": 1}):
            data = encode_indexed_png(**good, **kwargs)
            with self.assertRaises(ValueError):
                GEN.decode_indexed_png(data)

    def test_concatenates_multiple_idat_chunks(self):
        palette = [(i, i, i, 255) for i in range(4)]
        rows = [[0, 1, 2, 3], [3, 2, 1, 0]]
        whole = encode_indexed_png(4, 2, palette, rows, idat_parts=1)
        split = encode_indexed_png(4, 2, palette, rows, idat_parts=3)
        self.assertEqual(GEN.decode_indexed_png(split)[3], GEN.decode_indexed_png(whole)[3])

    def test_rejects_palette_index_out_of_range(self):
        data = encode_indexed_png(2, 1, [(1, 2, 3, 255), (4, 5, 6, 255)], [[0, 2]])
        with self.assertRaises(ValueError):
            GEN.decode_indexed_png(data)

    def test_rejects_truncated_chunk(self):
        data = encode_indexed_png(4, 3, [(1, 2, 3, 255)], [[0, 0, 0, 0]] * 3)
        with self.assertRaises(ValueError):
            GEN.decode_indexed_png(data[:-20])


class BboxAndFitTest(unittest.TestCase):
    def test_bbox_only_counts_pixels_at_or_above_the_alpha_threshold(self):
        # 2x2：只有右下角是 alpha=255，其余 alpha=100（低于门槛）或全透明。
        palette = [CLEAR, (10, 10, 10, 100), (20, 20, 20, 255)]
        _w, _h, pal, idx = GEN.decode_indexed_png(
            encode_indexed_png(2, 2, palette, [[1, 1], [1, 2]]))
        self.assertEqual(GEN.emoji_bbox(2, 2, pal, idx), (1, 1, 1, 1))

    def test_bbox_rejects_a_fully_transparent_image(self):
        palette = [(1, 2, 3, 255), CLEAR]
        _w, _h, pal, idx = GEN.decode_indexed_png(
            encode_indexed_png(2, 1, palette, [[1, 1]]))
        with self.assertRaises(ValueError):
            GEN.emoji_bbox(2, 1, pal, idx)

    def test_fit_keeps_the_aspect_ratio_and_centres(self):
        # 38x72 是气球的框：等比后 11x20，水平居中在 20 里（左 4 右 5）。
        self.assertEqual(GEN.fit_box((0, 0, 37, 71)), (0, 0, 38, 72, 11, 20, 4, 0))
        # 72x64 的框：20x18，垂直居中（上 1 下 1）。
        self.assertEqual(GEN.fit_box((0, 0, 71, 63)), (0, 0, 72, 64, 20, 18, 0, 1))

    def test_every_real_asset_fits_as_measured(self):
        for name, _label, code in GEN.EMOJI:
            width, height, palette, indices = real_asset(code)
            box = GEN.emoji_bbox(width, height, palette, indices)
            _x0, _y0, _w, _h, w2, h2, ox, oy = GEN.fit_box(box)
            self.assertEqual((w2, h2, ox, oy), FIT_EXPECTED[name], name)


class DownsampleTest(unittest.TestCase):
    def test_tie_goes_to_the_lower_rgb(self):
        """票数并列取 RGB 字典序较小的（全序、可复现，且偏向深色）。"""
        palette = [RED, BLUE]
        _w, _h, pal, idx = GEN.decode_indexed_png(
            encode_indexed_png(2, 1, palette, [[0, 1]]))
        grid = GEN.downsample_emoji(2, 1, pal, idx, (0, 0, 1, 0), n=1)
        self.assertEqual(grid, [[(0, 0, 255)]])

    def test_coverage_threshold_is_inclusive(self):
        """累计 alpha 恰好半格时算不透明（边界方向被钉住，防将来漂移）。"""
        palette = [(255, 0, 0, 255), CLEAR]
        # 2x2 里两个不透明两个透明：cover = 510 = 0.5 * 255 * 4 → 不透明。
        _w, _h, pal, idx = GEN.decode_indexed_png(
            encode_indexed_png(2, 2, palette, [[0, 1], [0, 1]]))
        self.assertEqual(GEN.downsample_emoji(2, 2, pal, idx, (0, 0, 1, 1), n=1),
                         [[(255, 0, 0)]])
        # 一个对三个：cover = 255 < 半格 → 透明。
        _w, _h, pal, idx = GEN.decode_indexed_png(
            encode_indexed_png(2, 2, palette, [[0, 1], [1, 1]]))
        self.assertEqual(GEN.downsample_emoji(2, 2, pal, idx, (0, 0, 1, 1), n=1),
                         [[None]])

    def test_votes_are_weighted_by_alpha_not_by_pixel_count(self):
        """投票权重是 alpha 累加，不是数像素个数。"""
        # 三个 alpha=128 的红（权重 384、个数 3）对两个 alpha=255 的蓝（权重 510、个数 2）：
        # 按个数红赢、按权重蓝赢——这里必须蓝赢。
        palette = [(255, 0, 0, 128), (0, 0, 255, 255)]
        _w, _h, pal, idx = GEN.decode_indexed_png(
            encode_indexed_png(5, 1, palette, [[0, 0, 0, 1, 1]]))
        self.assertEqual(GEN.downsample_emoji(5, 1, pal, idx, (0, 0, 4, 0), n=1),
                         [[(0, 0, 255)]])

    def test_pixels_below_the_alpha_threshold_do_not_vote(self):
        """alpha < 门槛的像素既不投票也不算覆盖。

        三个 alpha=127 的蓝若参与，这一格会被判成"蓝色不透明"（权重 381 > 红 255）；
        不参与时红的覆盖只有 255 < 半格 510，整格该是透明的。
        """
        palette = [(255, 0, 0, 255), (0, 0, 255, 127)]
        _w, _h, pal, idx = GEN.decode_indexed_png(
            encode_indexed_png(4, 1, palette, [[0, 1, 1, 1]]))
        self.assertEqual(GEN.downsample_emoji(4, 1, pal, idx, (0, 0, 3, 0), n=1), [[None]])


class IconInvariantsTest(unittest.TestCase):
    def test_palette_used_by_custom_avatars_is_frozen(self):
        self.assertEqual(GEN.PALETTE_ORDER, EXPECTED_PALETTE_ORDER)
        self.assertEqual(GEN.PALETTE, EXPECTED_PALETTE)
        expected_hex = ["%02X%02X%02X" % EXPECTED_PALETTE[ch] for ch in EXPECTED_PALETTE_ORDER]
        self.assertEqual(device_palette_hex(), expected_hex,
                         "love_pixel_palette 变了：已上传的自定义头像会全部换色")
        assets = json.loads((ROOT / "assets/images/web/assets.json").read_text(encoding="utf-8"))
        self.assertEqual(assets["palette"], ["#" + h for h in expected_hex],
                         "网页按这张表量化头像，和设备必须逐字一致")

    def test_icon_order_matches_the_device_header(self):
        header = (ROOT / "main" / "love_pixel_art.h").read_text(encoding="utf-8")
        for index, (name, _label, _code) in enumerate(GEN.EMOJI):
            self.assertRegex(header, rf"#define LOVE_ICON_{name.upper()} {index}\b",
                             "图标顺序是用户配置里存的图标号，改序 = 换掉用户的图标")
        self.assertRegex(header, r"#define LOVE_ICON_COUNT 16\b")
        self.assertRegex(header, r"#define LOVE_ICON_PX 40\b")

    def test_source_list_matches_the_fetch_manifest(self):
        manifest = json.loads((GEN.EMOJI_SRC_DIR / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual([(entry["name"], entry["file"]) for entry in manifest["files"]],
                         [(name, f"{code}.png") for name, _label, code in GEN.EMOJI])

    def test_every_real_asset_has_few_enough_colours(self):
        for name, _label, code in GEN.EMOJI:
            grid = GEN.load_emoji_grid(GEN.EMOJI_SRC_DIR / f"{code}.png")
            self.assertEqual(len(grid), GEN.EMOJI_PX)
            self.assertEqual(len(grid[0]), GEN.EMOJI_PX)
            icon_palette = GEN.per_icon_palette(grid)   # 超过 15 色会在这里抛错
            self.assertEqual(icon_palette[0], CLEAR, f"{name} 的索引 0 必须是透明")
            self.assertEqual(len(icon_palette), 16)
            data = GEN.pack_icon_i4(grid, icon_palette)
            self.assertEqual(len(data), 864, f"{name} 的 I4 数据必须恰好 864 字节")

    def test_pack_layout_matches_the_lvgl_indexed_convention(self):
        grid = [[RED[:3], None], [None, BLUE[:3]]]
        palette = GEN.per_icon_palette(grid)
        data = GEN.pack_icon_i4(grid, palette)
        side = 2 * GEN.EMOJI_SCALE
        self.assertEqual(len(data), 16 * 4 + side * side // 2)
        # 开头 16 项按 (B,G,R,A) 排；索引 0 恒为透明。
        self.assertEqual(palette[0], CLEAR)
        self.assertEqual(list(data[:4]), [0, 0, 0, 0])
        for i, (r, g, b, a) in enumerate(palette):
            self.assertEqual(list(data[i * 4:(i + 1) * 4]), [b, g, r, a],
                             f"palette[{i}] 的字节序必须是 B,G,R,A")
        # 索引数据每字节 2 像素、高半字节在前。每格放大 2 倍 → 每行 2 字节、
        # grid 的每一行占两条输出行：grid[0] 在偏移 0，grid[1] 在偏移 2 * row_bytes。
        red = palette.index(RED)
        blue = palette.index(BLUE)
        row_bytes = side // 2
        self.assertEqual(data[64], (red << 4) | red)
        self.assertEqual(data[64 + 1], 0x00, "grid[0] 右侧两格是透明")
        self.assertEqual(data[64 + 2 * row_bytes], 0x00, "grid[1] 左侧两格是透明")
        self.assertEqual(data[64 + 2 * row_bytes + 1], (blue << 4) | blue)

    def test_generate_is_deterministic(self):
        """跑两遍得到逐字节相同的产物（不许依赖 set/dict 的迭代顺序）。"""
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            a, b = Path(first), Path(second)
            self.assertEqual(GEN.generate(a), len(GEN.EMOJI))
            GEN.generate(b)
            for relative in ("assets/images/love_pixel_art.c", "main/love_pixel_art.h",
                             "assets/images/web/assets.json",
                             "assets/images/web/icons.json",
                             "assets/images/web/contact-sheet.png"):
                self.assertEqual((a / relative).read_bytes(), (b / relative).read_bytes(),
                                 relative)
            icons_json = json.loads((a / "assets/images/web/icons.json").read_text(encoding="utf-8"))
            self.assertEqual([entry["id"] for entry in icons_json],
                             [name for name, _label, _code in GEN.EMOJI])
            device_c = (a / "assets/images/love_pixel_art.c").read_text(encoding="utf-8")
            self.assertEqual(device_c.count("_data[864]"), len(GEN.EMOJI))


if __name__ == "__main__":
    unittest.main()
