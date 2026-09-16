#!/usr/bin/env python3
"""本地预览后台网页:用渲染后的模板 + 一份 mock /api/state,便于在电脑上核对版式。

用法(仓库根目录):
    python3 tools/preview_admin_page.py [端口]
"""

from __future__ import annotations

import base64
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "assets/web/admin.html"
ASSETS = ROOT / "assets/images/web/assets.json"
PIXEL_FONT = ROOT / "assets/fonts/ark12-subset.woff2"
LUNAR_TABLE = ROOT / "assets/images/web/lunar.json"

# 与设备 love_store_defaults / love_time 的返回结构保持一致。
MOCK_STATE = {
    "deviceName": "LoveCount-A1B2",
    "battery": 86,
    "config": {
        "start": "2000-01-01",
        "blankOff": 30,
        "people": [{"name": "咕咕", "icon": 0}, {"name": "嘎嘎", "icon": 1}],
        "events": [
            {"name": "元旦", "icon": 12, "kind": 0, "date": "2026-01-01"},
            {"name": "情人节", "icon": 6, "kind": 0, "date": "2026-02-14"},
            {"name": "咕咕嘎嘎", "icon": 10, "kind": 0, "date": "2000-01-01"},
            {"name": "国庆节", "icon": 7, "kind": 0, "date": "2026-10-01"},
            {"name": "圣诞节", "icon": 15, "kind": 0, "date": "2026-12-25"},
            # 农历事件用独立的 lunarMonth / lunarDay，不塞公历日期字段
            {"name": "春节", "icon": 11, "kind": 2, "lunarMonth": 1, "lunarDay": 1},
            {"name": "中秋", "icon": 9, "kind": 2, "lunarMonth": 8, "lunarDay": 15},
            {"name": "端午", "icon": 14, "kind": 2, "lunarMonth": 5, "lunarDay": 5},
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
        "ip": "192.168.1.23",
        "rssi": -52,
        "ssid": "home-2.4g",
        "apSsid": "LoveCount-A1B2",
        "apPass": "lovea1b2",
        "url": "http://192.168.4.1",
        "hasCredentials": True,
    },
    "icons": list(range(16)),
    "avatars": ["", "", "", ""],
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


def render_page() -> bytes:
    assets = json.loads(ASSETS.read_text(encoding="utf-8"))
    icons = [{"id": i["id"], "label": i["label"], "data": i["data"]} for i in assets["icons"]]
    html = TEMPLATE.read_text(encoding="utf-8")
    html = html.replace("__ICONS_JSON__", json.dumps(icons, ensure_ascii=False))
    html = html.replace("__BG_TILE_URI__", assets["bgTile"])
    palette = [[int(h[i:i + 2], 16) for i in (1, 3, 5)] for h in assets["palette"]]
    html = html.replace("__PALETTE_JSON__", json.dumps(palette))
    if LUNAR_TABLE.exists():
        html = html.replace("__LUNAR_JSON__", LUNAR_TABLE.read_text(encoding="utf-8").strip())
    # 与 gen_admin_page.py 保持一致:内嵌同一份像素字体子集,预览才和真机同字形。
    if PIXEL_FONT.exists():
        font_uri = ("data:font/woff2;base64,"
                    + base64.b64encode(PIXEL_FONT.read_bytes()).decode("ascii"))
        html = html.replace("__PIXEL_FONT_URI__", font_uri)
    else:
        html = html.replace("__PIXEL_FONT_URI__", "")
    return html.encode("utf-8")


PAGE = render_page()


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        if self.path == "/" :
            body, ctype = PAGE, "text/html; charset=utf-8"
        elif self.path == "/api/state":
            body, ctype = json.dumps(MOCK_STATE, ensure_ascii=False).encode("utf-8"), \
                "application/json; charset=utf-8"
        elif self.path == "/api/scan":
            body, ctype = json.dumps({"aps": [
                {"ssid": "home-2.4g", "rssi": -48, "secure": True},
                {"ssid": "FoloToy-A1B2", "rssi": -66, "secure": False},
            ]}, ensure_ascii=False).encode("utf-8"), "application/json; charset=utf-8"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):  # noqa: N802
        length = int(self.headers.get("Content-Length") or 0)
        payload = self.rfile.read(length) if length else b""
        path, _, query = self.path.partition("?")

        # 自定义头像:和真机一样按 slot 存定长 4bpp 数据,方便本地把上传流程走通。
        if path in ("/api/avatar", "/api/avatar/clear"):
            slot = _query_slot(query)
            if slot is None:
                return self._error(400, "slot 参数不合法")
            if path == "/api/avatar/clear":
                MOCK_STATE["avatars"][slot] = ""
            else:
                if len(payload) != 800:
                    return self._error(400, "头像数据必须是 800 字节的 4bpp 数据")
                MOCK_STATE["avatars"][slot] = base64.b64encode(payload).decode("ascii")

        body = json.dumps(MOCK_STATE, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _error(self, code: int, message: str) -> None:
        body = json.dumps({"error": message}, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    print(f"preview: http://127.0.0.1:{port}/ (Ctrl+C 结束)")
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
