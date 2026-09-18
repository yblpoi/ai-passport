#!/usr/bin/env node
// tests/test_avatar_slic.mjs —— 头像像素化内核(SLIC)的主机测试。
//
// 被保的东西:
//   1. **要发布的页面脚本能解析**:admin.js 的四个占位符按生成器的规则替换后,
//      整份脚本必须是一个合法的 classic script(重复声明、括号错位都会在这里炸);
//   2. 像素化的行为:纯色图不出现杂色、两块颜色分明的图边界干净、全透明给白、
//      同一输入两次结果一致(算法里不许有随机数);
//   3. 三档"像素化强度":越强色块越大、用到的颜色越少;
//   4. 4bpp 打包的半字节顺序(设备端按"高半字节在前"解)。
//
// 为什么用 node 而不是像其它主机测试那样用 C:这段逻辑最终跑在浏览器里,单独用 C
// 重写一遍测的是另一份实现。这里用 node 的 vm 直接加载 assets/web/avatar_slic.js
// ——测试跑的就是要发布的代码。
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import vm from "node:vm";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const WEB = path.join(ROOT, "assets/web");
const ASSETS = JSON.parse(fs.readFileSync(path.join(ROOT, "assets/images/web/assets.json"), "utf8"));

// 设备那 16 色(assets.json 里是 "#RRGGBB")。用它而不是自造一张表:量化结果是否
// 落在这张表上,正是"设备能不能按索引画出来"的条件。
const PALETTE = ASSETS.palette.map((hex) => [
  parseInt(hex.slice(1, 3), 16), parseInt(hex.slice(3, 5), 16), parseInt(hex.slice(5, 7), 16),
]);
assert.equal(PALETTE.length, 16, "调色板必须是 16 色");

const SLIC_SOURCE = fs.readFileSync(path.join(WEB, "avatar_slic.js"), "utf8");

/* ---------- 1. 要发布的页面脚本必须能解析 ---------- */

function renderedPageScript() {
  let script = fs.readFileSync(path.join(WEB, "admin.js"), "utf8");
  const icons = ASSETS.icons.map((i) => ({ label: i.label, data: i.data }));
  script = script.replace("__ICONS_JSON__", JSON.stringify(icons));
  script = script.replace("__PALETTE_JSON__", JSON.stringify(PALETTE));
  script = script.replace("__LUNAR_JSON__",
                          fs.readFileSync(path.join(ROOT, "assets/images/web/lunar.json"), "utf8").trim());
  script = script.replace("__AVATAR_SLIC_JS__", SLIC_SOURCE.trim());
  assert.ok(!script.includes("__AVATAR_SLIC_JS__"), "占位符没被替换掉");
  return script;
}

const pageScript = renderedPageScript();
new vm.Script(pageScript, { filename: "admin.js(inlined)" });   // 解析失败会抛

/* ---------- 内核:在干净的 vm 上下文里跑 ---------- */

const slic = vm.createContext({});
vm.runInContext(SLIC_SOURCE, slic, { filename: "avatar_slic.js" });
const slicPixelate = (px, params) =>
  vm.runInContext("slicPixelate", slic)(px, 40, PALETTE, params);
const packAvatar4bpp = vm.runInContext("packAvatar4bpp", slic);
const work = 40 * vm.runInContext("SLIC_WORK_SCALE", slic);

const RED = PALETTE.findIndex(([r, g, b]) => r > 200 && g < 80 && b < 80);
const BLUE = PALETTE.findIndex(([r, g, b]) => b > 200 && r < 80);
const GRAY = (() => {
  // 取调色板里"最接近中灰"的那一项,后面用它判断纯色图的输出。
  let best = 0, bestD = Infinity;
  for (let i = 0; i < PALETTE.length; i++) {
    const d = Math.abs(PALETTE[i][0] - 128) + Math.abs(PALETTE[i][1] - 128) + Math.abs(PALETTE[i][2] - 128);
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
})();

// 造一张工作分辨率(240x240)的图。fill(x, y) 返回 [r,g,b,a]。
function makeImage(fill) {
  const px = new Uint8ClampedArray(work * work * 4);
  for (let y = 0; y < work; y++) {
    for (let x = 0; x < work; x++) {
      const [r, g, b, a] = fill(x, y);
      const o = (y * work + x) * 4;
      px[o] = r; px[o + 1] = g; px[o + 2] = b; px[o + 3] = a;
    }
  }
  return px;
}

function countDistinct(idx) {
  return new Set(idx).size;
}

const results = [];

/* ---------- 2. 纯色 ---------- */
{
  // 一整张中灰:每个输出格子都该落在同一个索引上,而且是离中灰最近的那项。
  const px = makeImage(() => [128, 128, 128, 255]);
  const idx = slicPixelate(px, "normal");
  assert.equal(idx.length, 1600, "输出必须是 40x40 个索引");
  assert.equal(countDistinct(idx), 1, "纯色图不该出现第二种颜色");
  assert.equal(idx[0], GRAY, "纯色图必须落在离它最近的调色板项上");

  // 换一档也该是同一个索引:纯色没有"粒度"可言。
  assert.equal(slicPixelate(px, "strong")[0], GRAY);
  assert.equal(slicPixelate(px, "soft")[0], GRAY);
  results.push("纯色图不出现杂色");
}

/* ---------- 3. 两块颜色:边界要干净 ---------- */
{
  const px = makeImage((x) => (x < work / 2 ? [...PALETTE[RED], 255] : [...PALETTE[BLUE], 255]));
  const idx = slicPixelate(px, "normal");
  const bad = [...idx].filter((v) => v !== RED && v !== BLUE);
  assert.equal(bad.length, 0, `分明的两块不该出现第三种颜色,实际出现 ${bad.length} 个`);
  // 每一行都该是"左红右蓝",且分界在中间附近(允许 SLIC 把边界挪 1~2 个格子)。
  for (let y = 0; y < 40; y++) {
    const row = idx.slice(y * 40, (y + 1) * 40);
    const reds = [...row].filter((v) => v === RED).length;
    assert.ok(reds >= 18 && reds <= 22, `第 ${y} 行的红块宽 ${reds},偏离中线太多`);
    assert.ok(row.slice(0, 10).every((v) => v === RED), `第 ${y} 行左侧应为红`);
    assert.ok(row.slice(30).every((v) => v === BLUE), `第 ${y} 行右侧应为蓝`);
  }
  results.push("两块颜色的边界干净且落在中线");
}

/* ---------- 4. 全透明 ---------- */
{
  const px = makeImage(() => [0, 0, 0, 0]);
  const idx = slicPixelate(px, "normal");
  assert.ok([...idx].every((v) => v === 1), "全透明的图必须整张给白色(索引 1 = W)");
  results.push("全透明给白");
}

/* ---------- 5. 确定性 ---------- */
{
  // 带噪声的渐变:确定性一旦被破坏(比如引入随机播种),这里必挂。
  let seed = 12345;
  const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  const px = makeImage((x, y) => {
    const base = Math.round((x + y) / (2 * work) * 255);
    const n = Math.round((rnd() - 0.5) * 40);
    return [base + n, base, 255 - base, 255];
  });
  const a = slicPixelate(px, "normal");
  const b = slicPixelate(px, "normal");
  assert.deepEqual([...a], [...b], "同一张图两次必须完全一致");

  // 三档都要能跑通,且越强色块越大(用到的颜色越少)。
  const soft = slicPixelate(px, "soft");
  const normal = slicPixelate(px, "normal");
  const strong = slicPixelate(px, "strong");
  const nSoft = countDistinct(soft), nStrong = countDistinct(strong);
  assert.ok(nSoft >= nStrong,
            `"弱"用的颜色(${nSoft})不该少于"强"(${nStrong})`);

  /* ---------- 6. 4bpp 打包 ---------- */
  const packed = packAvatar4bpp(a);
  assert.equal(packed.length, 800, "40x40 的 4bpp 必须是 800 字节");
  assert.equal(packed[0] >> 4, a[0], "偶数像素在高半字节");
  assert.equal(packed[0] & 0x0F, a[1], "奇数像素在低半字节");
  assert.equal(packed[399] >> 4, a[798]);
  assert.equal(packed[399] & 0x0F, a[799]);
  for (const v of packed) assert.ok(v >= 0 && v <= 0xFF);
  results.push(`确定性 + 三档强度(soft ${nSoft} 色 / normal ${countDistinct(normal)} 色 / strong ${nStrong} 色) + 打包`);
}

/* ---------- 7. 上色方式与参数钳制 ---------- */
{
  // 同一张带噪声的图、同一套参数，只换上色方式:
  //   center / vote 必须比 mean 留下更多颜色(这正是用户说的"mean 完全没有细节");
  //   vote 又不能失控到把整张 16 色全用满得莫名其妙 —— 它仍然是"区域内的众数"。
  let seed = 99;
  const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  const px = makeImage((x, y) => {
    const base = Math.round((x + y) / (2 * work) * 200) + 30;
    const n = Math.round((rnd() - 0.5) * 30);
    return [base + n, base, 255 - base, 255];
  });

  const modes = ["mean", "center", "vote", "cell"];
  const out = {};
  for (const mode of modes) {
    const a = slicPixelate(px, { step: 14, iters: 6, weight: 21, mode });
    const b = slicPixelate(px, { step: 14, iters: 6, weight: 21, mode });
    assert.deepEqual([...a], [...b], `${mode} 必须可复现`);
    out[mode] = a;
  }
  const distinct = (idx) => new Set(idx).size;
  assert.ok(distinct(out.center) >= distinct(out.mean),
            `center 的颜色数(${distinct(out.center)}) 不该少于 mean(${distinct(out.mean)})`);
  assert.ok(distinct(out.vote) >= distinct(out.mean),
            `vote 的颜色数(${distinct(out.vote)}) 不该少于 mean(${distinct(out.mean)})`);

  // 参数钳制:越界值被夹住而不是把算法带进沟里;未知 mode 回落到默认。
  const resolved = vm.runInContext("resolveParams", slic);
  const clamped = resolved({ step: 9999, iters: -3, weight: 0, mode: "nope" });
  assert.ok(clamped.step <= 64 && clamped.step >= 2, "step 必须被夹到范围里");
  assert.ok(clamped.iters >= 1 && clamped.weight >= 1, "iters/weight 必须被夹到范围里");
  assert.equal(clamped.mode, vm.runInContext("DEFAULT_MODE", slic), "未知 mode 回落到默认");
  // 档位名与参数对象两条入口都要能用。
  assert.deepEqual(resolved("normal"), resolved({ step: 14, iters: 6, weight: 21,
                                                  mode: vm.runInContext("DEFAULT_MODE", slic) }));
  // cell 是默认模式：它按"格子内该区域的平均色"上色，是四个里细节最多的那个。
  assert.ok(distinct(out.cell) >= distinct(out.mean), "cell 不该比 mean 更平");
  results.push(`上色方式: mean ${distinct(out.mean)} / center ${distinct(out.center)} / vote ${distinct(out.vote)} / cell ${distinct(out.cell)} 色`);
}

// 生成脚本内联的那份也必须能解析(与上面 renderedPageScript 是同一件事的另一半:
// 确认 avatar_slic.js 自己作为 classic script 是合法的)。
new vm.Script(SLIC_SOURCE, { filename: "avatar_slic.js" });
results.push("页面脚本与内核都能解析");

console.log("Host tests (avatar SLIC): OK");
for (const line of results) console.log("  - " + line);
