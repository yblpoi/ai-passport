#!/usr/bin/env python3
"""把后台网页的三个模板文件编译成固件里的 C 头文件。

输入:
    assets/web/admin.html            —— 页面骨架(无占位符)
    assets/web/admin.css             —— 样式,引用 /bg.png
    assets/web/admin.js              —— 脚本(含 __ICONS_JSON__ / __PALETTE_JSON__ / __LUNAR_JSON__ 占位)
    assets/images/web/assets.json    —— 由 love_pixel_art_gen.py 生成的图标与底纹 PNG
    assets/images/web/lunar.json     —— 与设备端同一份农历表

输出:
    main/love_admin_page.h           —— HTML / CSS / JS 三个 C 字符串
    main/love_web_assets.h           —— 底纹 PNG 的原始字节

资源分成三个文本 + 一张底纹,是因为设备发不完大响应。页面曾经把 122 KB 的像素字体
以 base64 内联在 HTML 里,单次响应 214923 字节,在热点上必然撞上 socket 发送超时
(httpd_sock_err: error in send : 11)。字体现在整个去掉了(预览用系统字体);
图标反过来内联回 JS —— 它们只有两千多字节,拆成十六个请求只会把堆压到发不出帧。

用法(仓库根目录):
    python3 tools/gen_admin_page.py
"""

from __future__ import annotations

import base64
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "assets/web"
TEMPLATE = WEB / "admin.html"
STYLESHEET = WEB / "admin.css"
SCRIPT = WEB / "admin.js"
ASSETS = ROOT / "assets/images/web/assets.json"
LUNAR_TABLE = ROOT / "assets/images/web/lunar.json"
TEXT_OUTPUT = ROOT / "main/love_admin_page.h"
BLOB_OUTPUT = ROOT / "main/love_web_assets.h"

ICONS_PLACEHOLDER = "__ICONS_JSON__"
PALETTE_PLACEHOLDER = "__PALETTE_JSON__"
LUNAR_PLACEHOLDER = "__LUNAR_JSON__"

# C 字符串字面量按段拼接,避免单行过长让编译器难受。
CHUNK = 800
# 原始字节按 24 字节一行写,行宽约 100 列。
BLOB_PER_LINE = 24


def c_escape(text: str) -> str:
    return text.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t")


def to_c_literal(text: str) -> str:
    """把文本转成 C 字符串字面量序列,长行按 CHUNK 切开后隐式拼接。"""
    out = []
    for line in text.splitlines():
        escaped = c_escape(line)
        if not escaped:
            out.append('    "\\n"')
            continue
        for i in range(0, len(escaped), CHUNK):
            piece = escaped[i:i + CHUNK]
            # 最后一段才补 \n,拼出的字符串与原文本逐字节一致。
            tail = "\\n" if i + CHUNK >= len(escaped) else ""
            out.append(f'    "{piece}{tail}"')
    return "\n".join(out)


def to_c_bytes(data: bytes) -> str:
    """把二进制转成 C 字符串字面量。每个字节都写成 \\xNN,不存在转义边界歧义。"""
    out = []
    for i in range(0, len(data), BLOB_PER_LINE):
        chunk = data[i:i + BLOB_PER_LINE]
        out.append('    "' + "".join(f"\\x{byte:02x}" for byte in chunk) + '"')
    return "\n".join(out)


def decode_png(data_uri: str, what: str) -> bytes:
    """把 assets.json 里的 data:image/png;base64,... 解回原始 PNG 字节。"""
    prefix = "data:image/png;base64,"
    if not data_uri.startswith(prefix):
        raise ValueError(f"{what} 不是 PNG data URI")
    return base64.b64decode(data_uri[len(prefix):])


def render_script(script: str, assets: dict) -> str:
    # 图标以 data URI 内联在脚本里。它们一共只有两千多字节,拆成 /icon/N.png 会让
    # 一次页面加载多出十六个并发请求,把本来就紧的堆压到发不出帧(实测最大连续块
    # 掉到 1280 字节,低于 Wi-Fi 驱动发一帧所需的 ~1600)。图标不是体积瓶颈:
    # 当初让页面打不开的是 167KB 的像素字体,那个已经删掉了。
    icons = [{"label": icon["label"], "data": icon["data"]} for icon in assets["icons"]]
    script = script.replace(ICONS_PLACEHOLDER, json.dumps(icons, ensure_ascii=False))
    # 16 色调色板:网页按它把上传的图片量化成 4bpp,顺序必须与设备端一致。
    palette = [[int(h[i:i + 2], 16) for i in (1, 3, 5)] for h in assets["palette"]]
    script = script.replace(PALETTE_PLACEHOLDER, json.dumps(palette))
    # 农历表:与设备端同一份数据,预览才不会算出和真机差一天的日子。
    script = script.replace(LUNAR_PLACEHOLDER, LUNAR_TABLE.read_text(encoding="utf-8").strip())

    for placeholder in (ICONS_PLACEHOLDER, PALETTE_PLACEHOLDER, LUNAR_PLACEHOLDER):
        if placeholder in script:
            raise ValueError(f"脚本占位符 {placeholder} 未替换")
    return script


def write_text_header(html: str, css: str, script: str) -> None:
    TEXT_OUTPUT.write_text(
        "// main/love_admin_page.h —— 由 tools/gen_admin_page.py 生成,请勿手改。\n"
        "// 模板在 assets/web/admin.{html,css,js},字体与图标见 main/love_web_assets.h。\n"
        "// 三个资源分开返回,理由见生成脚本的文档字符串(单次响应太大会被 socket 超时掐断)。\n"
        "#pragma once\n"
        "\n"
        "static const char LOVE_ADMIN_HTML[] =\n"
        f"{to_c_literal(html)};\n"
        "#define LOVE_ADMIN_HTML_SIZE (sizeof(LOVE_ADMIN_HTML) - 1)\n"
        "\n"
        "static const char LOVE_ADMIN_CSS[] =\n"
        f"{to_c_literal(css)};\n"
        "#define LOVE_ADMIN_CSS_SIZE (sizeof(LOVE_ADMIN_CSS) - 1)\n"
        "\n"
        "static const char LOVE_ADMIN_JS[] =\n"
        f"{to_c_literal(script)};\n"
        "#define LOVE_ADMIN_JS_SIZE (sizeof(LOVE_ADMIN_JS) - 1)\n"
        "\n",
        encoding="utf-8")


def write_blob_header(bg_tile: bytes) -> None:
    BLOB_OUTPUT.write_text(
        "// main/love_web_assets.h —— 由 tools/gen_admin_page.py 生成,请勿手改。\n"
        "// 底纹来自 assets/images/love_pixel_art_gen.py,后台网页与设备界面用的是\n"
        "// 同一份素材。这是原始字节(不是 base64),以 /bg.png 分块发给浏览器。\n"
        "// 图标不在这里:它们太小,直接以 data URI 内联在 admin.js 里更省连接数。\n"
        "#pragma once\n"
        "\n"
        "#include <stdint.h>\n"
        "\n"
        "static const uint8_t LOVE_WEB_BG_PNG[] =\n"
        f"{to_c_bytes(bg_tile)};\n"
        "#define LOVE_WEB_BG_PNG_SIZE (sizeof(LOVE_WEB_BG_PNG) - 1)\n"
        "\n",
        encoding="utf-8")


def main() -> int:
    for path in (TEMPLATE, STYLESHEET, SCRIPT, ASSETS, LUNAR_TABLE):
        if not path.exists():
            print(f"缺少 {path.relative_to(ROOT)},请先生成素材/农历表", file=sys.stderr)
            return 1

    html = TEMPLATE.read_text(encoding="utf-8")
    css = STYLESHEET.read_text(encoding="utf-8")
    assets = json.loads(ASSETS.read_text(encoding="utf-8"))
    script = render_script(SCRIPT.read_text(encoding="utf-8"), assets)

    for text, name in ((html, TEMPLATE), (css, STYLESHEET)):
        for placeholder in (ICONS_PLACEHOLDER, PALETTE_PLACEHOLDER, LUNAR_PLACEHOLDER):
            if placeholder in text:
                print(f"{name.name} 里出现了只属于 admin.js 的占位符 {placeholder}",
                      file=sys.stderr)
                return 1

    bg_tile = decode_png(assets["bgTile"], "底纹")

    write_text_header(html, css, script)
    write_blob_header(bg_tile)

    print(f"admin page: {TEXT_OUTPUT.relative_to(ROOT)} "
          f"(HTML {len(html.encode())} + CSS {len(css.encode())} + "
          f"JS {len(script.encode())} 字节) + "
          f"{BLOB_OUTPUT.relative_to(ROOT)} (底纹 {len(bg_tile)} B,"
          f"{len(assets['icons'])} 个图标已内联进 JS)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
