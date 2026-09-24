#!/usr/bin/env node
// tests/test_preview_math.mjs —— 后台页预览内核(preview_math.js)的主机测试。
//
// 后台页要在手机上看预览,所以"下一次发生日""农历折算""在一起多少天"这些规则在 JS 里
// 又实现了一遍。这里的向量**不是手写的期望值**,是 tools/gen_date_vectors.py 调设备端
// C 实现(love_date.c / love_lunar.c)跑出来的:tests/vectors/date_vectors.json。
// 于是两边一比,谁漂了立刻红 —— 这个测试真的抓到过两处(农历 廿一/二十一、
// 今天早于起始日时的天数),不是形式主义。
//
// 向量过期也算失败:改了 C 实现却没重新生成,这里核对的就是旧向量。
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import vm from "node:vm";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const VECTORS = path.join(ROOT, "tests/vectors/date_vectors.json");

const lunarText = fs.readFileSync(path.join(ROOT, "assets/images/web/lunar.json"), "utf8").trim();
const source = fs.readFileSync(path.join(ROOT, "assets/web/preview_math.js"), "utf8")
                     .replace("__LUNAR_JSON__", lunarText);

const context = vm.createContext({});
vm.runInContext(source, context, { filename: "preview_math.js" });
const fn = (name) => vm.runInContext(name, context);
const parseDate = fn("parseDate");
const daysBetween = fn("daysBetween");
const nextOccurrence = fn("nextOccurrence");
const lunarToSolar = fn("lunarToSolar");
const nextLunar = fn("nextLunar");
const lunarName = fn("lunarName");

const doc = JSON.parse(fs.readFileSync(VECTORS, "utf8"));
const cases = doc.cases;
assert.ok(doc.note && doc.note.includes("gen_date_vectors.py"),
          "向量文件缺少生成说明:它必须是生成的,不是手写的");

const fmt = (d) => d ? `${d.y}-${String(d.m).padStart(2, "0")}-${String(d.d).padStart(2, "0")}` : null;
let checks = 0;

function same(actual, expected, what) {
  checks++;
  assert.deepEqual(actual, expected, `${what}\n  实际 ${JSON.stringify(actual)}\n  向量 ${JSON.stringify(expected)}`);
}

/* ---------- 1. 事件倒计时:与设备端 love_event_countdown 逐条对齐 ---------- */

for (const c of cases.events) {
  const today = parseDate(c.today);
  const where = `${c.kind} today=${c.today} m=${c.month} d=${c.day} date=${c.date}`;

  // 与 admin.js 的 renderPreview 用同一套调用:每年 -> nextOccurrence;
  // 仅一次 -> 直接解析日期;农历 -> nextLunar(折算不出就是"超出范围")。
  let target = null;
  let resolved = true;
  if (c.kind === "yearly") {
    target = nextOccurrence(today, c.month, c.day);
  } else if (c.kind === "once") {
    target = parseDate(c.date);
  } else {
    target = nextLunar(today, c.month, c.day);
    resolved = target !== null;
  }

  same(resolved, c.resolved, `算不算得出来不符: ${where}`);
  same(fmt(target), c.target, `目标日不符: ${where}`);
  same(target ? daysBetween(today, target) : null, c.days, `天数不符: ${where}`);
}

/* ---------- 2. "在一起 N 天":含首日、且不显示负数 ---------- */

for (const c of cases.days_together) {
  const start = parseDate(c.start);
  const today = parseDate(c.today);
  // 与设备端 love_days_together 一致:起始日当天 = 1,今天早于起始日 = 0。
  same(Math.max(0, daysBetween(start, today) + 1), c.days,
       `在一起天数不符: ${c.start} -> ${c.today}`);
}

/* ---------- 3. 农历换算 ---------- */

for (const c of cases.lunar_solar) {
  same(fmt(lunarToSolar(c.year, c.month, c.day)), c.solar,
       `农历 ${c.year}-${c.month}-${c.day} 折算不符`);
}

/* ---------- 4. 农历中文写法(含「二十X」与越界回落) ---------- */

for (const c of cases.lunar_names) {
  same(lunarName(c.month, c.day), c.text, `农历写法不符: ${c.month} 月 ${c.day} 日`);
}

console.log(`preview_math: ${checks} 项与设备端向量一致`);
console.log(`  - 事件倒计时 ${cases.events.length} 条(每年 / 仅一次 / 农历,含算不出的年份)`);
console.log(`  - 在一起天数 ${cases.days_together.length} 条(含跨闰年与"今天早于起始日")`);
console.log(`  - 农历换算 ${cases.lunar_solar.length} 条(含表两端与越界)`);
console.log(`  - 农历写法 ${cases.lunar_names.length} 条(含「二十一」这类设备专用写法)`);
