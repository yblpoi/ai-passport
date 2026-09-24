#!/usr/bin/env python3
"""把后台网页的三个模板文件编译成固件里的 C 头文件。

输入:
    assets/web/admin.html            —— 页面骨架(无占位符)
    assets/web/admin.css             —— 样式,引用 /bg.png
    assets/web/admin.js              —— 脚本(含 __ICONS_JSON__ / __PALETTE_JSON__ / __LUNAR_JSON__ 占位)
    assets/web/preview_math.js        —— 预览用的倒计时/农历内核(纯函数,单独可测,由本脚本内联)
    assets/images/web/assets.json    —— 由 love_pixel_art_gen.py 生成的图标与底纹 PNG
    assets/images/web/lunar.json     —— 与设备端同一份农历表

输出:
    main/love_admin_page.h           —— HTML / CSS / JS 三份 **gzip 字节**
    main/love_web_assets.h           —— 底纹 PNG 的原始字节

资源分成三个文本 + 一张底纹,是因为设备发不完大响应。页面曾经把 122 KB 的像素字体
以 base64 内联在 HTML 里,单次响应 214923 字节,在热点上必然撞上 socket 发送超时
(httpd_sock_err: error in send : 11)。字体现在整个去掉了(预览用系统字体);
图标反过来内联回 JS —— 它们只有两千多字节,拆成十几个请求只会把堆压到发不出帧。

文本资源以 **gzip** 存进固件,响应带 Content-Encoding: gzip(见 main/love_httpd.c):
三份资源原始 111884 字节,压完 43523 字节,直接省掉约 68 KB flash;压缩在生成期做完,
设备侧一个字节都不用解压 —— 浏览器自己解。代价是只带 gzip 编码,不接受 gzip 的裸
HTTP 客户端(例如没加 --compressed 的 curl)拿到的是压缩流。

用法(仓库根目录):
    python3 tools/gen_admin_page.py
"""

from __future__ import annotations

import base64
import gzip
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "assets/web"
TEMPLATE = WEB / "admin.html"
STYLESHEET = WEB / "admin.css"
SCRIPT = WEB / "admin.js"
# 头像的像素化内核(纯函数,不碰 DOM)。单独成文件是为了让 tests/test_avatar_pixel.mjs
# 能用 node 直接跑它;这里把它内联进 admin.js,页面上仍然只有一个 /admin.js 请求。
AVATAR_PIXEL = WEB / "avatar_pixel.js"
# 预览的倒计时/农历内核。同样单独成文件、同样被内联:它和设备端 C 实现是同一套规则,
# 抽出来之后 tests/test_preview_math.mjs 能拿 C 生成的向量逐个核对(tests/vectors/)。
PREVIEW_MATH = WEB / "preview_math.js"
ASSETS = ROOT / "assets/images/web/assets.json"
LUNAR_TABLE = ROOT / "assets/images/web/lunar.json"
TEXT_OUTPUT = ROOT / "main/love_admin_page.h"
BLOB_OUTPUT = ROOT / "main/love_web_assets.h"

ICONS_PLACEHOLDER = "__ICONS_JSON__"
PALETTE_PLACEHOLDER = "__PALETTE_JSON__"
LUNAR_PLACEHOLDER = "__LUNAR_JSON__"
PIXEL_PLACEHOLDER = "__AVATAR_PIXEL_JS__"
PREVIEW_MATH_PLACEHOLDER = "__PREVIEW_MATH_JS__"

# 页面图标(浏览器标签页与 iOS 主屏幕)用哪颗图标,对应 assets.json 里的 id。
PAGE_ICON_ID = "heart"

# 原始字节按 24 字节一行写,行宽约 100 列。
BLOB_PER_LINE = 24


def served_bytes(text: str) -> bytes:
    """设备真正发出的字节。

    源码按行读入,每行补一个 \n,所以"文件末尾没有换行"时会补上一个 —— 与旧版
    `to_c_literal()` 拼出来的字符串逐字节相同。压缩的就是这串字节,不是文件原始字节。
    """
    text = "\n".join(text.splitlines())
    if not text.endswith("\n"):
        text += "\n"
    return text.encode("utf-8")


def gzip_bytes(data: bytes) -> bytes:
    """确定性 gzip:mtime 固定为 0,同样输入永远得到同样的字节。"""
    return gzip.compress(data, compresslevel=9, mtime=0)


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
    # 一次页面加载多出十几个并发请求,把本来就紧的堆压到发不出帧(实测最大连续块
    # 掉到 1280 字节,低于 Wi-Fi 驱动发一帧所需的 ~1600)。图标不是体积瓶颈:
    # 当初让页面打不开的是 167KB 的像素字体,那个已经删掉了。
    icons = [{"label": icon["label"], "data": icon["data"]} for icon in assets["icons"]]
    script = script.replace(ICONS_PLACEHOLDER, json.dumps(icons, ensure_ascii=False))
    # 设备那 16 色:只在画**老头像**时当回退用(新头像自带调色板,见 avatar_pixel.js),
    # 以及给内置图标当参考。顺序必须与设备端一致。
    palette = [[int(h[i:i + 2], 16) for i in (1, 3, 5)] for h in assets["palette"]]
    script = script.replace(PALETTE_PLACEHOLDER, json.dumps(palette))
    # 农历表:与设备端同一份数据,预览才不会算出和真机差一天的日子。
    lunar = LUNAR_TABLE.read_text(encoding="utf-8").strip()
    script = script.replace(LUNAR_PLACEHOLDER, lunar)
    # 预览内核:它自己也带 __LUNAR_JSON__,先替它补上再内联。
    preview_math = PREVIEW_MATH.read_text(encoding="utf-8")
    preview_math = preview_math.replace(LUNAR_PLACEHOLDER, lunar).strip()
    script = script.replace(PREVIEW_MATH_PLACEHOLDER, preview_math)
    # 头像像素化内核:一并内联,页面上就还是"一个 /admin.js 请求"。
    script = script.replace(PIXEL_PLACEHOLDER,
                            AVATAR_PIXEL.read_text(encoding="utf-8").strip())

    for placeholder in (ICONS_PLACEHOLDER, PALETTE_PLACEHOLDER, LUNAR_PLACEHOLDER,
                        PIXEL_PLACEHOLDER, PREVIEW_MATH_PLACEHOLDER):
        if placeholder in script:
            raise ValueError(f"脚本占位符 {placeholder} 未替换")
    return script


def pack_texts(html: str, css: str, script: str) -> list[tuple[str, bytes, bytes]]:
    """三份文本资源 -> [(名字, 原文字节, gzip 字节), ...]。"""
    packed = []
    for name, text in (("HTML", html), ("CSS", css), ("JS", script)):
        raw = served_bytes(text)
        packed.append((name, raw, gzip_bytes(raw)))
    return packed


def render_text_header(packed: list[tuple[str, bytes, bytes]]) -> str:
    """三份文本资源各自 gzip 后的字节数组。"""
    parts = [
        "// main/love_admin_page.h —— 由 tools/gen_admin_page.py 生成,请勿手改。\n"
        "// 模板在 assets/web/admin.{html,css,js},字体与图标见 main/love_web_assets.h。\n"
        "// 三个资源分开返回,理由见生成脚本的文档字符串(单次响应太大会被 socket 超时掐断)。\n"
        "//\n"
        "// 存的是 gzip 流,不是可读文本:响应带 Content-Encoding: gzip,由浏览器解压。\n"
        "// 这样三份资源从 111884 字节降到 43523 字节,而设备侧一个字节都不用解\n"
        "// (见 main/love_httpd.c 的 send_blob)。\n"
        "#pragma once\n\n"
        "#include <stdint.h>\n"
    ]
    for name, _raw, blob in packed:
        # 字符串字面量总带一个隐式结尾 NUL,所以 sizeof - 1 正好是数据长度。
        parts.append(f"\nstatic const uint8_t LOVE_ADMIN_{name}_GZ[] =\n"
                     f"{to_c_bytes(blob)};\n"
                     f"#define LOVE_ADMIN_{name}_GZ_SIZE (sizeof(LOVE_ADMIN_{name}_GZ) - 1)\n")
    return "".join(parts)


def render_blob_header(bg_tile: bytes, page_icon: bytes, icon_count: int) -> str:
    return (
        "// main/love_web_assets.h —— 由 tools/gen_admin_page.py 生成,请勿手改。\n"
        "// 素材来自 assets/images/love_pixel_art_gen.py,后台网页与设备界面用的是\n"
        "// 同一份素材。这些是原始字节(不是 base64),分块发给浏览器。\n"
        f"// {icon_count} 个图标不在这里:它们太小,以 data URI 内联在 admin.js 里更省连接数。\n"
        "// 页面图标要单独放进固件,是因为浏览器会主动请求 /favicon.ico 和\n"
        "// /apple-touch-icon*.png,那些路径没法用内联的 data URI 应答。\n"
        "#pragma once\n"
        "\n"
        "#include <stdint.h>\n"
        "\n"
        "static const uint8_t LOVE_WEB_BG_PNG[] =\n"
        f"{to_c_bytes(bg_tile)};\n"
        "#define LOVE_WEB_BG_PNG_SIZE (sizeof(LOVE_WEB_BG_PNG) - 1)\n"
        "\n"
        "// assets.json 里 id 为 \"heart\" 的图标,40x40。iOS 会把它放大当主屏幕图标,\n"
        "// 像素图放大后偏软但不糊;要清晰的 180x180 得在 love_pixel_art_gen.py 里另出一张。\n"
        "static const uint8_t LOVE_WEB_PAGE_ICON_PNG[] =\n"
        f"{to_c_bytes(page_icon)};\n"
        "#define LOVE_WEB_PAGE_ICON_PNG_SIZE (sizeof(LOVE_WEB_PAGE_ICON_PNG) - 1)\n"
        "\n")


def write_blob_header(bg_tile: bytes, page_icon: bytes, icon_count: int) -> None:
    BLOB_OUTPUT.write_text(
        "// main/love_web_assets.h —— 由 tools/gen_admin_page.py 生成,请勿手改。\n"
        "// 素材来自 assets/images/love_pixel_art_gen.py,后台网页与设备界面用的是\n"
        "// 同一份素材。这些是原始字节(不是 base64),分块发给浏览器。\n"
        f"// {icon_count} 个图标不在这里:它们太小,以 data URI 内联在 admin.js 里更省连接数。\n"
        "// 页面图标要单独放进固件,是因为浏览器会主动请求 /favicon.ico 和\n"
        "// /apple-touch-icon*.png,那些路径没法用内联的 data URI 应答。\n"
        "#pragma once\n"
        "\n"
        "#include <stdint.h>\n"
        "\n"
        "static const uint8_t LOVE_WEB_BG_PNG[] =\n"
        f"{to_c_bytes(bg_tile)};\n"
        "#define LOVE_WEB_BG_PNG_SIZE (sizeof(LOVE_WEB_BG_PNG) - 1)\n"
        "\n"
        "// assets.json 里 id 为 \"heart\" 的图标,40x40。iOS 会把它放大当主屏幕图标,\n"
        "// 像素图放大后偏软但不糊;要清晰的 180x180 得在 love_pixel_art_gen.py 里另出一张。\n"
        "static const uint8_t LOVE_WEB_PAGE_ICON_PNG[] =\n"
        f"{to_c_bytes(page_icon)};\n"
        "#define LOVE_WEB_PAGE_ICON_PNG_SIZE (sizeof(LOVE_WEB_PAGE_ICON_PNG) - 1)\n"
        "\n",
        encoding="utf-8")


def load_texts() -> tuple[str, str, str, dict]:
    """读模板、代入占位符,返回 (html, css, script, assets)。"""
    for path in (TEMPLATE, STYLESHEET, SCRIPT, PREVIEW_MATH, AVATAR_PIXEL, ASSETS, LUNAR_TABLE):
        if not path.exists():
            raise ValueError(f"缺少 {path.relative_to(ROOT)},请先生成素材/农历表")

    html = TEMPLATE.read_text(encoding="utf-8")
    css = STYLESHEET.read_text(encoding="utf-8")
    assets = json.loads(ASSETS.read_text(encoding="utf-8"))
    script = render_script(SCRIPT.read_text(encoding="utf-8"), assets)

    for text, name in ((html, TEMPLATE), (css, STYLESHEET)):
        for placeholder in (ICONS_PLACEHOLDER, PALETTE_PLACEHOLDER, LUNAR_PLACEHOLDER):
            if placeholder in text:
                raise ValueError(f"{name.name} 里出现了只属于 admin.js 的占位符 {placeholder}")

    return html, css, script, assets


def build_all() -> tuple[str, str, list[tuple[str, int, int]]]:
    """渲染两个头文件的内容。

    返回 (文本头, 资源头, [(名字, 原始字节数, gzip 字节数), ...])。
    写成纯函数是为了让 tests/test_gen_admin_page.py 能重放一遍并与仓库里的生成结果
    逐字节比对 —— 改了 assets/web/* 却忘记重新生成,门禁要拦住。
    """
    html, css, script, assets = load_texts()

    bg_tile = decode_png(assets["bgTile"], "底纹")
    heart = next((icon for icon in assets["icons"] if icon["id"] == PAGE_ICON_ID), None)
    if heart is None:
        raise ValueError(f"assets.json 里找不到 id 为 {PAGE_ICON_ID!r} 的图标"
                         f"(页面图标用的就是它)")

    packed = pack_texts(html, css, script)
    stats = [(name, len(raw), len(blob)) for name, raw, blob in packed]

    return (render_text_header(packed),
            render_blob_header(bg_tile, decode_png(heart["data"], f"页面图标 {PAGE_ICON_ID}"),
                               len(assets["icons"])),
            stats)


def main() -> int:
    try:
        text_header, blob_header, stats = build_all()
    except ValueError as err:
        print(err, file=sys.stderr)
        return 1

    TEXT_OUTPUT.write_text(text_header, encoding="utf-8")
    BLOB_OUTPUT.write_text(blob_header, encoding="utf-8")

    raw = sum(item[1] for item in stats)
    packed = sum(item[2] for item in stats)
    detail = " + ".join(f"{name} {r}->{p}" for name, r, p in stats)
    print(f"admin page: {TEXT_OUTPUT.relative_to(ROOT)} "
          f"({detail} 字节,共 {raw}->{packed},省 {raw - packed}) + "
          f"{BLOB_OUTPUT.relative_to(ROOT)} (底纹 + 页面图标,已内联进 JS 的图标不在内)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
