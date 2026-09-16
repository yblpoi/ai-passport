#!/usr/bin/env python3
"""从权威农历库导出设备端用的紧凑农历表，并用公开来源的春节日期反查校验。

产出：main/love_lunar_table.h

表格式（沿用通行做法，每个农历年一个 20 bit 值）：
    bit 0..3  闰月月份，0 表示当年无闰月
    bit 4..15 正月..十二月，该位为 1 表示当月 30 天，0 表示 29 天
    bit 16    闰月天数：1 = 30 天，0 = 29 天
锚点：LOVE_LUNAR_BASE_YEAR 年的正月初一 = 公历 LOVE_LUNAR_BASE_MONTH/DAY

校验方式：脚本末尾用生成的表推算每一年的春节，与写死在
KNOWN_SPRING_FESTIVAL 里的公开来源日期逐个比对，不一致就退出非零、不写文件。

用法（需要 `pip install lunarcalendar`）：
    python3 tools/gen_lunar_table.py
"""

from __future__ import annotations

import datetime
import json
import sys
from pathlib import Path

from lunarcalendar import Converter, Lunar, Solar

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "main/love_lunar_table.h"
WEB_OUTPUT = ROOT / "assets/images/web/lunar.json"

BASE_YEAR = 2018
END_YEAR = 2050
BASE_SOLAR = datetime.date(2018, 2, 16)   # 2018 年正月初一（公开来源）

# 两个独立来源（chinesecalendaronline / rili.com.cn）一致的春节公历日期。
# 表一旦算错，这里会立刻报出来 —— 农历算错比不做更糟，所以必须逐条对。
KNOWN_SPRING_FESTIVAL = {
    2018: (2, 16), 2019: (2, 5), 2020: (1, 25), 2021: (2, 12),
    2022: (2, 1), 2023: (1, 22), 2024: (2, 10), 2025: (1, 29),
    2026: (2, 17), 2027: (2, 6), 2028: (1, 26), 2029: (2, 13),
    2030: (2, 3), 2031: (1, 23), 2032: (2, 11), 2033: (1, 31),
    2034: (2, 19), 2035: (2, 8),
}

# 另外几个非正月初一的农历节日，用来验"月内位置"也对（不只是年初对）。
KNOWN_FESTIVAL_DATES = {
    (2026, 5, 5): (6, 19),    # 端午
    (2026, 8, 15): (9, 25),   # 中秋
}


def solar_of(year: int, month: int, day: int, leap: bool = False) -> datetime.date:
    s: Solar = Converter.Lunar2Solar(Lunar(year, month, day, leap))
    return datetime.date(s.year, s.month, s.day)


def leap_month_of(year: int) -> int:
    """当年闰月号，0 表示无闰月。逐个试，能转出公历日期且回读仍是闰月才算。"""
    for m in range(1, 13):
        try:
            solar: Solar = Converter.Lunar2Solar(Lunar(year, m, 1, True))
        except Exception:
            continue
        back: Lunar = Converter.Solar2Lunar(solar)
        if back.isleap and back.month == m:
            return m
    return 0


def year_lengths(year: int, leap_month: int) -> list[int]:
    """按 正月..腊月（闰月插在其后）给出每月天数。"""
    sequence: list[tuple[int, bool]] = []
    for m in range(1, 13):
        sequence.append((m, False))
        if leap_month == m:
            sequence.append((m, True))

    starts = [solar_of(year, m, 1, leap) for m, leap in sequence]
    starts.append(solar_of(year + 1, 1, 1, False))
    return [(starts[i + 1] - starts[i]).days for i in range(len(sequence))]


def pack(year: int, leap_month: int, lengths: list[int]) -> int:
    value = leap_month & 0xF
    normal: list[int] = []
    leap_days = 0
    idx = 0
    for m in range(1, 13):
        normal.append(lengths[idx])
        idx += 1
        if leap_month == m:
            leap_days = lengths[idx]
            idx += 1
    if idx != len(lengths):
        raise AssertionError(f"{year}: 月数对不上 {idx} != {len(lengths)}")

    for i, days in enumerate(normal):
        if days not in (29, 30):
            raise AssertionError(f"{year} 第{i + 1}月天数异常: {days}")
        if days == 30:
            value |= 1 << (4 + i)

    if leap_month:
        if leap_days not in (29, 30):
            raise AssertionError(f"{year} 闰月天数异常: {leap_days}")
        if leap_days == 30:
            value |= 1 << 16
    return value


def main() -> int:
    table: list[int] = []
    lengths_by_year: dict[int, list[int]] = {}
    leap_by_year: dict[int, int] = {}

    for y in range(BASE_YEAR, END_YEAR + 1):
        leap = leap_month_of(y)
        lengths = year_lengths(y, leap)
        leap_by_year[y] = leap
        lengths_by_year[y] = lengths
        table.append(pack(y, leap, lengths))

    errors: list[str] = []

    # 除夕是腊月最后一天，这里顺手记下每年的腊月天数供 C 侧用（不写进表，C 侧自己算）。
    for year, (m, d) in sorted(KNOWN_SPRING_FESTIVAL.items()):
        days = sum(sum(lengths_by_year[y]) for y in range(BASE_YEAR, year))
        got = BASE_SOLAR + datetime.timedelta(days=days)
        if (got.month, got.day) != (m, d):
            errors.append(f"  春节 {year}: 表算出 {got.month:02d}-{got.day:02d}，公开来源 {m:02d}-{d:02d}")

    for (year, lm, ld), (m, d) in sorted(KNOWN_FESTIVAL_DATES.items()):
        days = sum(sum(lengths_by_year[y]) for y in range(BASE_YEAR, year))
        seq_idx = 0
        for month in range(1, lm):
            seq_idx += 1
            if leap_by_year[year] == month:
                seq_idx += 1
        days += sum(lengths_by_year[year][:seq_idx]) + (ld - 1)
        got = BASE_SOLAR + datetime.timedelta(days=days)
        if (got.month, got.day) != (m, d):
            errors.append(f"  农历 {year}-{lm}-{ld}: 表算出 {got.month:02d}-{got.day:02d}，公开来源 {m:02d}-{d:02d}")

    if errors:
        print("农历表反查不通过，未写文件：", file=sys.stderr)
        print("\n".join(errors), file=sys.stderr)
        return 1

    lines = [
        "// main/love_lunar_table.h —— 由 tools/gen_lunar_table.py 生成，请勿手改。",
        "//",
        "// 农历年数据表。每年一个 20 bit 值：",
        "//   bit 0..3  闰月月份（0 = 当年无闰月）",
        "//   bit 4..15 正月..十二月（该位为 1 表示当月 30 天，0 表示 29 天）",
        "//   bit 16    闰月天数（1 = 30 天，0 = 29 天）",
        "//",
        "// 生成时已用公开来源的春节日期逐年反查（见 tools/gen_lunar_table.py）。",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"// 覆盖 {BASE_YEAR}..{END_YEAR} 年；超出范围时 love_lunar 返回失败，不做猜测。",
        f"#define LOVE_LUNAR_BASE_YEAR {BASE_YEAR}",
        f"#define LOVE_LUNAR_YEARS {END_YEAR - BASE_YEAR + 1}",
        f"// 锚点：{BASE_YEAR} 年正月初一 = 公历 {BASE_SOLAR.year}-{BASE_SOLAR.month:02d}-{BASE_SOLAR.day:02d}",
        f"#define LOVE_LUNAR_BASE_MONTH {BASE_SOLAR.month}",
        f"#define LOVE_LUNAR_BASE_DAY {BASE_SOLAR.day}",
        "",
        "static const uint32_t LOVE_LUNAR_INFO[LOVE_LUNAR_YEARS] = {",
    ]
    for i, value in enumerate(table):
        year = BASE_YEAR + i
        leap = leap_by_year[year]
        tag = f"闰{leap}月" if leap else "无闰"
        days = sum(lengths_by_year[year])
        lines.append(f"    0x{value:05X},  // {year} {tag} 全年 {days} 天")
    lines += ["};", ""]

    OUTPUT.write_text("\n".join(lines), encoding="utf-8")

    # 后台网页也要同一张表：实测浏览器的 Intl 中国农历在 18 个年份里有 2 个偏 ±1 天，
    # 不能拿来当预览依据。给网页同一份数据，两端跑同一套算法才不会各说各话。
    WEB_OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    WEB_OUTPUT.write_text(
        json.dumps({
            "baseYear": BASE_YEAR,
            "baseMonth": BASE_SOLAR.month,
            "baseDay": BASE_SOLAR.day,
            "info": [f"0x{v:05X}" for v in table],
        }, ensure_ascii=False, indent=1) + "\n",
        encoding="utf-8")

    print(f"{OUTPUT.relative_to(ROOT)}: {BASE_YEAR}..{END_YEAR} 共 {len(table)} 年")
    print(f"{WEB_OUTPUT.relative_to(ROOT)}: 同表给后台网页用")
    print(f"反查通过：春节 {len(KNOWN_SPRING_FESTIVAL)} 个年份 + "
          f"其他农历节日 {len(KNOWN_FESTIVAL_DATES)} 条全部一致")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
