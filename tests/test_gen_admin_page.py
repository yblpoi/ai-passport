#!/usr/bin/env python3
"""Host tests for the admin-page generator.

钉住三件事:

1. **生成物与素材同步** —— 改了 assets/web/* 却忘记跑生成器,门禁必须拦住。
   固件里编译的是 main/love_admin_page.h,它落后于模板时,真机上跑的还是旧页面
   (这条坑踩过不止一次)。
2. **gzip 往返** —— 内嵌的字节解压后与设备应该发出的文本逐字节相等;压缩流一旦写坏,
   浏览器拿到的是乱码而设备看不出来。
3. **体积预算** —— 三份资源合计压完的字节数有个上限,防止它们悄悄长回去。
"""

from __future__ import annotations

import gzip
import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("gen_admin_page",
                                              ROOT / "tools" / "gen_admin_page.py")
assert SPEC and SPEC.loader
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)

# 三份资源合计的 gzip 上限。当前实测 43523 字节;留一点余量,但别让页面再长回去 ——
# 这一块每多 1 KB 就是 flash 上实打实的 1 KB。要改大,先想清楚多出来的字节买到了什么。
PACKED_BUDGET = 60_000


class GeneratedHeadersTest(unittest.TestCase):
    def test_headers_match_the_templates(self) -> None:
        text_header, blob_header, _stats = GEN.build_all()
        for path, expected in ((GEN.TEXT_OUTPUT, text_header), (GEN.BLOB_OUTPUT, blob_header)):
            with self.subTest(path=path.name):
                actual = path.read_text(encoding="utf-8")
                self.assertEqual(
                    actual, expected,
                    f"{path.relative_to(ROOT)} 与 assets/web/* 不同步,"
                    f"请运行 python3 tools/gen_admin_page.py 后一起提交")


class PackedAssetsTest(unittest.TestCase):
    def test_round_trip(self) -> None:
        html, css, script, _assets = GEN.load_texts()
        packed = GEN.pack_texts(html, css, script)
        for name, raw, blob in packed:
            with self.subTest(asset=name):
                self.assertEqual(gzip.decompress(blob), raw)
                self.assertLess(len(blob), len(raw))
                # mtime 固定为 0:同样输入永远得到同样的字节,生成物才可比对。
                self.assertEqual(blob[4:8], b"\x00\x00\x00\x00")

    def test_total_size_budget(self) -> None:
        html, css, script, _assets = GEN.load_texts()
        packed = GEN.pack_texts(html, css, script)
        total = sum(len(blob) for _name, _raw, blob in packed)
        self.assertLessEqual(
            total, PACKED_BUDGET,
            f"后台页 gzip 后共 {total} 字节,超过预算 {PACKED_BUDGET};"
            f"确认这笔体积花得值,再改 tests/test_gen_admin_page.py 的上限")


if __name__ == "__main__":
    unittest.main()
