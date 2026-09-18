#!/usr/bin/env python3
"""抓取内置图标用的 Twemoji 素材（只在更新素材时手工跑，不参与构建）。

Twemoji 的图形（`assets/72x72/*.png`）以 CC-BY 4.0 发布，署名与许可原文见
`assets/images/emoji/LICENSE-GRAPHICS.txt`，项目侧说明见 `assets/README(.zh_CN).md`。

这里按**固定 tag** 下载（不接受分支名，分支上的字节随时会变），下载完把
sha256 写进 `assets/images/emoji/manifest.json`；之后任何人重跑这个脚本，
字节对不上就非零退出。`--check` 只校验不下载。

用法（仓库根目录）：
    python3 assets/images/fetch_emoji.py           # 下载并校验/登记
    python3 assets/images/fetch_emoji.py --check   # 只校验已有素材
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEST = ROOT / "assets/images/emoji"
MANIFEST = DEST / "manifest.json"

REPOSITORY = "https://github.com/jdecked/twemoji"
TAG = "v16.0.0"
BASE_URL = f"https://raw.githubusercontent.com/jdecked/twemoji/{TAG}/assets/72x72"
LICENSE_URL = f"https://raw.githubusercontent.com/jdecked/twemoji/{TAG}/LICENSE-GRAPHICS"

# 顺序即 love_pixel_art.h 里 LOVE_ICON_* 的下标，绝不能改（用户的配置里存着下标）。
# 每项: (Twemoji 文件名, 图标名, 码位)
EMOJI: list[tuple[str, str, str]] = [
    ("1f426", "bird", "U+1F426"),
    ("1f431", "cat", "U+1F431"),
    ("1f436", "dog", "U+1F436"),
    ("1f430", "rabbit", "U+1F430"),
    ("1f43b", "bear", "U+1F43B"),
    ("1f98a", "fox", "U+1F98A"),
    ("2764", "heart", "U+2764"),
    ("2b50", "star", "U+2B50"),
    ("1f338", "flower", "U+1F338"),
    ("1f96e", "moon", "U+1F96E"),
    ("1f382", "cake", "U+1F382"),
    ("1f381", "gift", "U+1F381"),
    ("1f388", "balloon", "U+1F388"),
    ("1f48d", "ring", "U+1F48D"),
    ("1f970", "loving", "U+1F970"),
    ("1f384", "tree", "U+1F384"),
]

LICENSE_FILE = "LICENSE-GRAPHICS.txt"

# 写进 manifest，给下游（以及许可审核）一个明确交代：我们改了什么。
LOCAL_MODIFICATION = (
    "Pixelated for the 240x320 display: transparent margins trimmed, scaled "
    "proportionally to a 20x20 logical grid (alpha-weighted dominant colour per "
    "cell), then upscaled 2x to 40x40 by assets/images/love_pixel_art_gen.py."
)


def fetch(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "ai-passport-fetch-emoji"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return response.read()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_manifest() -> dict | None:
    if not MANIFEST.exists():
        return None
    return json.loads(MANIFEST.read_text(encoding="utf-8"))


def verify(dest: Path, data: bytes, expected: str, what: str) -> None:
    actual = sha256(data)
    if actual != expected:
        raise SystemExit(
            f"sha256 不符: {what}\n  期望 {expected}\n  实际 {actual}\n"
            f"上游素材变了或本地文件被改过；确认无误后删掉 manifest.json 重新登记。")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="只校验，不下载")
    args = parser.parse_args()

    manifest = read_manifest()
    known = {entry["file"]: entry["sha256"] for entry in manifest["files"]} if manifest else {}

    DEST.mkdir(parents=True, exist_ok=True)

    license_bytes = (DEST / LICENSE_FILE).read_bytes() if args.check else fetch(LICENSE_URL)
    if manifest:
        verify(DEST / LICENSE_FILE, license_bytes, manifest["license_sha256"], LICENSE_FILE)
    if not args.check:
        (DEST / LICENSE_FILE).write_bytes(license_bytes)

    entries = []
    for code, name, codepoint in EMOJI:
        file_name = f"{code}.png"
        if args.check:
            data = (DEST / file_name).read_bytes()
        else:
            data = fetch(f"{BASE_URL}/{file_name}")
        digest = sha256(data)
        if known:
            verify(DEST / file_name, data, known[file_name], file_name)
        if not args.check:
            (DEST / file_name).write_bytes(data)
        entries.append({"file": file_name, "code": codepoint, "name": name, "sha256": digest})
        print(f"{file_name}  {len(data):5d} B  {digest[:16]}…")

    if not manifest:
        MANIFEST.write_text(json.dumps({
            "source": "Twemoji",
            "repository": REPOSITORY,
            "tag": TAG,
            "license": "CC-BY 4.0",
            "license_file": LICENSE_FILE,
            "license_sha256": sha256(license_bytes),
            "local_modification": LOCAL_MODIFICATION,
            "files": entries,
        }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"登记 {MANIFEST.relative_to(ROOT)}")
    else:
        print(f"校验通过：{DEST.relative_to(ROOT)} 与 manifest.json 一致")


if __name__ == "__main__":
    sys.exit(main())
