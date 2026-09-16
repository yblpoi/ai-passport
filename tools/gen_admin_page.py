#!/usr/bin/env python3
"""把后台网页模板编译成固件里的 C 头文件。

输入:
    assets/web/admin.html            —— 页面模板(含 __ICONS_JSON__ / __BG_TILE_URI__ / __PIXEL_FONT_URI__ 占位)
    assets/images/web/assets.json    —— 由 love_pixel_art_gen.py 生成的图标与底纹数据 URI
    assets/fonts/ark12-subset.woff2  —— 方舟像素字体 12px 的网页子集(与设备端同一套字形)

输出:
    main/love_admin_page.h           —— `LOVE_ADMIN_HTML`,直接由 HTTP 服务器返回

用法(仓库根目录):
    python3 tools/gen_admin_page.py
"""

from __future__ import annotations

import base64
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "assets/web/admin.html"
ASSETS = ROOT / "assets/images/web/assets.json"
PIXEL_FONT = ROOT / "assets/fonts/ark12-subset.woff2"
LUNAR_TABLE = ROOT / "assets/images/web/lunar.json"
OUTPUT = ROOT / "main/love_admin_page.h"

ICONS_PLACEHOLDER = "__ICONS_JSON__"
BG_PLACEHOLDER = "__BG_TILE_URI__"
FONT_PLACEHOLDER = "__PIXEL_FONT_URI__"
PALETTE_PLACEHOLDER = "__PALETTE_JSON__"
LUNAR_PLACEHOLDER = "__LUNAR_JSON__"

# C 字符串字面量按段拼接。base64 子集有十几万字符,塞成单独一行会让编译器很难受。
CHUNK = 800


def c_escape(text: str) -> str:
    return text.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t")


def to_c_literal(html: str) -> str:
    """把 HTML 转成 C 字符串字面量序列,长行按 CHUNK 切开后隐式拼接。"""
    out = []
    for line in html.splitlines():
        escaped = c_escape(line)
        if not escaped:
            out.append('    "\\n"')
            continue
        for i in range(0, len(escaped), CHUNK):
            piece = escaped[i:i + CHUNK]
            last = i + CHUNK >= len(escaped)
            out.append(f'    "{piece}{"\\n" if last else ""}"')
    return "\n".join(out)


def main() -> int:
    for path in (TEMPLATE, ASSETS, PIXEL_FONT, LUNAR_TABLE):
        if not path.exists():
            print(f"缺少 {path.relative_to(ROOT)},请先生成素材/字体/农历表", file=sys.stderr)
            return 1

    html = TEMPLATE.read_text(encoding="utf-8")
    assets = json.loads(ASSETS.read_text(encoding="utf-8"))
    icons = [{"id": i["id"], "label": i["label"], "data": i["data"]} for i in assets["icons"]]

    font_uri = ("data:font/woff2;base64,"
                + base64.b64encode(PIXEL_FONT.read_bytes()).decode("ascii"))
    lunar_text = LUNAR_TABLE.read_text(encoding="utf-8").strip()

    html = html.replace(ICONS_PLACEHOLDER, json.dumps(icons, ensure_ascii=False))
    html = html.replace(BG_PLACEHOLDER, assets["bgTile"])
    html = html.replace(FONT_PLACEHOLDER, font_uri)
    # 16 色调色板:网页按它把上传的图片量化成 4bpp,顺序必须与设备端一致。
    palette = [[int(h[i:i + 2], 16) for i in (1, 3, 5)] for h in assets["palette"]]
    html = html.replace(PALETTE_PLACEHOLDER, json.dumps(palette))
    # 农历表:与设备端同一份数据,预览才不会算出和真机差一天的日子。
    html = html.replace(LUNAR_PLACEHOLDER, lunar_text)

    for placeholder in (ICONS_PLACEHOLDER, BG_PLACEHOLDER, FONT_PLACEHOLDER,
                        PALETTE_PLACEHOLDER, LUNAR_PLACEHOLDER):
        if placeholder in html:
            print(f"模板占位符 {placeholder} 未替换", file=sys.stderr)
            return 1

    OUTPUT.write_text(
        "// main/love_admin_page.h —— 由 tools/gen_admin_page.py 生成,请勿手改。\n"
        "// 页面模板在 assets/web/admin.html;图标与底纹来自 assets/images/love_pixel_art_gen.py,\n"
        "// 字体来自 assets/fonts/ark12-subset.woff2,因此后台网页与设备界面用的是同一套素材与字形。\n"
        "#pragma once\n"
        "\n"
        "static const char LOVE_ADMIN_HTML[] =\n"
        f"{to_c_literal(html)};\n"
        "\n"
        "#define LOVE_ADMIN_HTML_SIZE (sizeof(LOVE_ADMIN_HTML) - 1)\n"
        "\n",
        encoding="utf-8")

    print(f"admin page: {OUTPUT.relative_to(ROOT)} "
          f"{OUTPUT.stat().st_size // 1024} KB "
          f"({len(icons)} icons + {PIXEL_FONT.stat().st_size // 1024} KB 像素字体内嵌)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
