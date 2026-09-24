#!/usr/bin/env python3
"""从**设备端 C 实现**生成后台页预览的测试向量。

后台页要在手机上看预览,所以它必须在 JS 里再实现一遍"下一次发生日""农历折算"这些规则。
两份实现迟早会漂 —— 实际上已经漂过两处(农历 廿一/二十一、今天早于起始日时的天数)。
这个脚本把设备端实现跑一遍、把结果写成向量,页面那份 JS 用同一批向量自检:

    main/love_date.c + main/love_lunar.c  →  tools/date_vectors_dump.c
        →  tests/vectors/date_vectors.json  →  tests/test_preview_math.mjs

用法(仓库根目录):
    python3 tools/gen_date_vectors.py            # 重新生成
    python3 tools/gen_date_vectors.py --check    # 只检查仓库里那份是不是最新的
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DUMPER = ROOT / "tools" / "date_vectors_dump.c"
VECTORS = ROOT / "tests" / "vectors" / "date_vectors.json"
SOURCES = [ROOT / "main" / "love_date.c", ROOT / "main" / "love_lunar.c"]
NOTE = ("由 tools/gen_date_vectors.py 从设备端 C 实现(love_date.c / love_lunar.c)生成,"
        "不要手改;改规则请改 C 实现后重新生成。")


def build_vectors() -> dict:
    """编译并运行 dumper,返回排好版的向量文档。"""
    with tempfile.TemporaryDirectory(prefix="ai-passport-date-vectors-") as tmp:
        binary = Path(tmp) / "date_vectors_dump"
        subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             f"-I{ROOT / 'main'}", str(DUMPER), *[str(s) for s in SOURCES],
             "-o", str(binary)],
            check=True, cwd=ROOT)
        result = subprocess.run([str(binary)], check=True, capture_output=True,
                                text=True, cwd=ROOT)

    return {"note": NOTE, "cases": json.loads(result.stdout)}


def rendered(document: dict) -> str:
    return json.dumps(document, ensure_ascii=False, indent=2, sort_keys=False) + "\n"


def main() -> int:
    check_only = "--check" in sys.argv[1:]
    try:
        document = build_vectors()
    except subprocess.CalledProcessError as err:
        print(f"生成向量失败: {err}", file=sys.stderr)
        return 1

    text = rendered(document)
    if check_only:
        if not VECTORS.exists():
            print(f"缺少 {VECTORS.relative_to(ROOT)},"
                  f"请运行 python3 tools/gen_date_vectors.py", file=sys.stderr)
            return 1
        if VECTORS.read_text(encoding="utf-8") != text:
            print(f"{VECTORS.relative_to(ROOT)} 与设备端实现不一致,"
                  f"请运行 python3 tools/gen_date_vectors.py 后一起提交", file=sys.stderr)
            return 1
        print(f"date vectors: 最新({len(text.splitlines())} 行)")
        return 0

    VECTORS.parent.mkdir(parents=True, exist_ok=True)
    VECTORS.write_text(text, encoding="utf-8")
    cases = document["cases"]
    print(f"date vectors: {VECTORS.relative_to(ROOT)} "
          f"(事件 {len(cases['events'])} + 天数 {len(cases['days_together'])} + "
          f"农历换算 {len(cases['lunar_solar'])} + 农历写法 {len(cases['lunar_names'])})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
