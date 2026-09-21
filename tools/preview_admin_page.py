#!/usr/bin/env python3
"""本地预览后台网页:用渲染后的模板 + 一份 mock /api/state,便于在电脑上核对版式。

资源的路由与设备端一致(/admin.css、/admin.js、/bg.png),所以这里跑起来和真机
是同一套请求,改了拆分方式也能立刻发现。图标是内联的 data URI,不走请求。

用法(仓库根目录):
    python3 tools/preview_admin_page.py [端口]
"""

from __future__ import annotations

import base64
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "assets/web"
TEMPLATE = WEB / "admin.html"
STYLESHEET = WEB / "admin.css"
SCRIPT = WEB / "admin.js"
AVATAR_PIXEL = ROOT / "assets" / "web" / "avatar_pixel.js"
ASSETS = ROOT / "assets/images/web/assets.json"
LUNAR_TABLE = ROOT / "assets/images/web/lunar.json"

# 与设备 love_store_defaults / love_time 的返回结构保持一致。
MOCK_STATE = {
    "deviceName": "LoveCount-A1B2",
    "battery": 86,
    "config": {
        "start": "2000-01-01",
        "blankOff": 30,
        "bleEnabled": False,
        "people": [{"name": "咕咕", "icon": 0}, {"name": "嘎嘎", "icon": 1}],
        # viewMode:v4 起每条事件自己挑展示方式 —— 0 = 进列表屏,1 = 自己占一屏。
        # 这里刻意两种都放,本地预览就能同时看到列表/单页的差别。
        "events": [
            {"name": "元旦", "icon": 12, "kind": 0, "date": "2026-01-01", "category": "节假日", "viewMode": 0},
            {"name": "情人节", "icon": 6, "kind": 0, "date": "2026-02-14", "viewMode": 0},
            # 与 love_store.c 的出厂占位生日一致:假日期 + 占位名字。
            {"name": "咕咕嘎嘎", "icon": 10, "kind": 0, "date": "2000-01-01", "viewMode": 1},
            {"name": "国庆节", "icon": 7, "kind": 0, "date": "2026-10-01", "viewMode": 0},
            {"name": "圣诞节", "icon": 15, "kind": 0, "date": "2026-12-25", "viewMode": 0},
            # 农历事件用独立的 lunarMonth / lunarDay，不塞公历日期字段
            {"name": "春节", "icon": 11, "kind": 2, "lunarMonth": 1, "lunarDay": 1, "viewMode": 0},
            {"name": "中秋", "icon": 9, "kind": 2, "lunarMonth": 8, "lunarDay": 15, "viewMode": 0},
            {"name": "端午", "icon": 14, "kind": 2, "lunarMonth": 5, "lunarDay": 5, "viewMode": 0},
        ],
    },
    "time": {
        "synced": True,
        "epoch": 1789560000,
        "sourceText": "网页对时",
        "text": "09-16 20:00 网页对时",
    },
    "net": {
        "state": "connected",
        "stateText": "已联网",
        "ap": True,
        "apManualOff": False,
        "ip": "192.168.1.23",
        "rssi": -52,
        "ssid": "home-2.4g",
        "apSsid": "LoveCount-A1B2",
        "apPass": "lovea1b2",
        "url": "http://192.168.4.1",
        "lanUrl": "http://10.255.234.34",
        "hasCredentials": True,
    },
    "icons": list(range(16)),
    "avatars": ["", "", "", ""],
    "avatar_palettes": ["", "", "", ""],
}


def _decode_png(data_uri: str) -> bytes:
    return base64.b64decode(data_uri.split(",", 1)[1])


ASSETS_JSON = json.loads(ASSETS.read_text(encoding="utf-8"))


def _render_pages() -> tuple[bytes, bytes, bytes]:
    """按 gen_admin_page.py 的同一套占位符规则渲染三个文本资源。

    占位符清单必须与 tools/gen_admin_page.py 一致 —— 少替换一个,页面会在浏览器里
    直接抛 ReferenceError(实测踩过:头像内核没被内联,整页脚本停在第 32 行)。
    生成器那边有"占位符未替换就报错"的自检,这里用同一个常量表兜住。
    """
    icons = [{"label": icon["label"], "data": icon["data"]} for icon in ASSETS_JSON["icons"]]
    palette = [[int(h[i:i + 2], 16) for i in (1, 3, 5)] for h in ASSETS_JSON["palette"]]

    script = SCRIPT.read_text(encoding="utf-8")
    script = script.replace("__ICONS_JSON__", json.dumps(icons, ensure_ascii=False))
    script = script.replace("__PALETTE_JSON__", json.dumps(palette))
    script = script.replace("__LUNAR_JSON__", LUNAR_TABLE.read_text(encoding="utf-8").strip())
    script = script.replace("__AVATAR_PIXEL_JS__", AVATAR_PIXEL.read_text(encoding="utf-8").strip())

    for placeholder in ("__ICONS_JSON__", "__PALETTE_JSON__", "__LUNAR_JSON__",
                        "__AVATAR_PIXEL_JS__"):
        if placeholder in script:
            raise SystemExit(f"预览渲染漏了占位符 {placeholder}")

    return (TEMPLATE.read_text(encoding="utf-8").encode("utf-8"),
            STYLESHEET.read_text(encoding="utf-8").encode("utf-8"),
            script.encode("utf-8"))


PAGE, CSS, JS = _render_pages()

# 与设备端 love_httpd.c 的静态资源路由一一对应。页面图标在设备端是 heart 那颗,
# 浏览器会自己去要 /favicon.ico 和 /apple-touch-icon*.png,这里一并接住。
_PAGE_ICON = next(i for i in ASSETS_JSON["icons"] if i["id"] == "heart")
STATIC = {
    "/admin.css": (CSS, "text/css; charset=utf-8"),
    "/admin.js": (JS, "application/javascript; charset=utf-8"),
    "/bg.png": (_decode_png(ASSETS_JSON["bgTile"]), "image/png"),
    "/favicon.ico": (_decode_png(_PAGE_ICON["data"]), "image/png"),
    "/apple-touch-icon.png": (_decode_png(_PAGE_ICON["data"]), "image/png"),
    "/apple-touch-icon-precomposed.png": (_decode_png(_PAGE_ICON["data"]), "image/png"),
}


def _query_slot(query: str):
    """从 "slot=N" 里取出槽位号;取不到或越界返回 None。"""
    for part in query.split("&"):
        key, _, value = part.partition("=")
        if key == "slot":
            try:
                slot = int(value)
            except ValueError:
                return None
            return slot if 0 <= slot < 4 else None
    return None


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        if self.path == "/":
            return self._send(200, PAGE, "text/html; charset=utf-8")
        if self.path == "/api/state":
            return self._send(200, json.dumps(MOCK_STATE, ensure_ascii=False).encode("utf-8"),
                              "application/json; charset=utf-8")
        if self.path == "/api/scan":
            return self._send(200, json.dumps({"aps": [
                {"ssid": "home-2.4g", "rssi": -48, "secure": True},
                {"ssid": "FoloToy-A1B2", "rssi": -66, "secure": False},
            ]}, ensure_ascii=False).encode("utf-8"), "application/json; charset=utf-8")
        if self.path in STATIC:
            body, ctype = STATIC[self.path]
            return self._send(200, body, ctype)

        self.send_error(404)

    def do_POST(self):  # noqa: N802
        length = int(self.headers.get("Content-Length") or 0)
        payload = self.rfile.read(length) if length else b""
        path, _, query = self.path.partition("?")

        # 保存配置:写回 mock,这样"改顺序/分类/展示模式 → 保存 → 重新载入"在本地
        # 就能看到结果。真机的 handle_config 还会对**缺失**字段保留原值(浏览器缓存的
        # 旧 admin.js 不带 displayMode / category),本页的 admin.js 每次都发全量字段,
        # 所以这里直接整体替换 —— 那条兼容逻辑要靠真机验。
        if path == "/api/config":
            try:
                incoming = json.loads(payload.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                return self._error(400, "请求体不是合法 JSON")
            if not isinstance(incoming, dict):
                return self._error(400, "请求体必须是对象")
            MOCK_STATE["config"] = incoming

        # 自定义头像:和真机一样按 slot 存,方便本地把上传流程走通。
        # 864 字节 = 64 字节配色 + 800 字节索引(新格式);800 字节 = 只用索引,配色清空
        # ——与 main/love_httpd.c 的 handle_avatar() 同一套规则。
        if path in ("/api/avatar", "/api/avatar/clear"):
            slot = _query_slot(query)
            if slot is None:
                return self._error(400, "slot 参数不合法")
            if path == "/api/avatar/clear":
                MOCK_STATE["avatars"][slot] = ""
                MOCK_STATE["avatar_palettes"][slot] = ""
            else:
                if len(payload) not in (800, 864):
                    return self._error(400, "头像数据必须是 864 字节(64 配色 + 800 索引)或 800 字节(只用索引)")
                has_palette = len(payload) == 864
                if has_palette:
                    MOCK_STATE["avatar_palettes"][slot] = base64.b64encode(payload[:64]).decode("ascii")
                else:
                    MOCK_STATE["avatar_palettes"][slot] = ""
                MOCK_STATE["avatars"][slot] = base64.b64encode(payload[-800:]).decode("ascii")

        # 热点开关:真机的 /api/ap 会改设备状态并把新状态回给网页。这里照做,
        # 否则"关热点 → 提示文案变成手动关闭"这条分支在本地根本走不到。
        if path == "/api/ap":
            try:
                incoming = json.loads(payload.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                return self._error(400, "请求体不是合法 JSON")
            on = bool(incoming.get("on")) if isinstance(incoming, dict) else False
            MOCK_STATE["net"]["ap"] = on
            MOCK_STATE["net"]["apManualOff"] = not on
            MOCK_STATE["net"]["url"] = "http://192.168.4.1" if on else ""

        body = json.dumps(MOCK_STATE, ensure_ascii=False).encode("utf-8")
        self._send(200, body, "application/json; charset=utf-8")

    def _send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        # 本地预览一律禁缓存:改完 admin.js / admin.css 普通刷新拿到的还是旧文件,
        # 会让人以为改动没生效(真机那边靠固件版本号变化,不存在这个问题)。
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _error(self, code: int, message: str) -> None:
        body = json.dumps({"error": message}, ensure_ascii=False).encode("utf-8")
        self._send(code, body, "application/json; charset=utf-8")

    def log_message(self, *_args):
        pass


class PreviewServer(ThreadingHTTPServer):
    """浏览器会一口气开好几个连接,默认 5 个的 listen backlog 会把多余的连接拒掉
    (表现为控制台里的 ERR_CONNECTION_RESET / ERR_SOCKET_NOT_CONNECTED),
    看起来像资源缺失。真机那边靠 max_open_sockets = 6 + lwIP 的队列接住。"""

    daemon_threads = True
    request_queue_size = 64
    allow_reuse_address = True


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    print(f"preview: http://127.0.0.1:{port}/ (Ctrl+C 结束)")
    PreviewServer(("127.0.0.1", port), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
