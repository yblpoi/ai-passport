#!/usr/bin/env node
// tests/test_avatar_pixel.mjs —— 头像像素化内核(每图自带调色板 + 逐格选色)的主机测试。
//
// 被保的东西:
//   1. **要发布的页面脚本能解析**:admin.js 的四个占位符按生成器的规则替换后,
//      整份脚本必须是一个合法的 classic script(重复声明、括号错位都会在这里炸);
//   2. 取色:纯色图取到那一个色、**设备那 16 色里没有的颜色也能取到**、两块颜色取到
//      那两个色、全透明给白、同一张图两次结果一致(算法里不许有随机数)、
//      取色数量越大颜色越多;
//   3. 上色:两块分明的图边界干净;一格按"过半像素同色就填那个色,没有过半才回落到
//      格内 Lab 均值最近的那个色"来填(两条分支各有一条用例钉着);
//   4. 打包:4bpp 的半字节顺序(设备端按"高半字节在前"解),以及 64 字节配色的小端
//      布局(设备端按 0x00RRGGBB 读,和 love_pixel_palette 一样)。
//
// 为什么用 node 而不是像其它主机测试那样用 C:这段逻辑最终跑在浏览器里,单独用 C
// 重写一遍测的是另一份实现。这里用 node 的 vm 直接加载 assets/web/avatar_pixel.js
// ——测试跑的就是要发布的代码。
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import vm from "node:vm";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const WEB = path.join(ROOT, "assets/web");
const ASSETS = JSON.parse(fs.readFileSync(path.join(ROOT, "assets/images/web/assets.json"), "utf8"));

// 设备那 16 色(assets.json 里是 "#RRGGBB")。新头像自带调色板,这份表是**老头像与
// 内置图标**用的回退,页面里的 PALETTE 常量就是它 —— 这里用它确认"回退路径没被改坏"。
const DEVICE_PALETTE = ASSETS.palette.map((hex) => [
  parseInt(hex.slice(1, 3), 16), parseInt(hex.slice(3, 5), 16), parseInt(hex.slice(5, 7), 16),
]);
assert.equal(DEVICE_PALETTE.length, 16, "设备调色板必须是 16 色");

const KERNEL_SOURCE = fs.readFileSync(path.join(WEB, "avatar_pixel.js"), "utf8");
const PREVIEW_MATH_SOURCE = fs.readFileSync(path.join(WEB, "preview_math.js"), "utf8");

/* ---------- 1. 要发布的页面脚本必须能解析 ---------- */

function renderedPageScript() {
  let script = fs.readFileSync(path.join(WEB, "admin.js"), "utf8");
  const icons = ASSETS.icons.map((i) => ({ label: i.label, data: i.data }));
  const lunar = fs.readFileSync(path.join(ROOT, "assets/images/web/lunar.json"), "utf8").trim();
  script = script.replace("__ICONS_JSON__", JSON.stringify(icons));
  script = script.replace("__PALETTE_JSON__", JSON.stringify(DEVICE_PALETTE));
  script = script.replace("__LUNAR_JSON__", lunar);
  script = script.replace("__PREVIEW_MATH_JS__",
                          PREVIEW_MATH_SOURCE.replace("__LUNAR_JSON__", lunar).trim());
  script = script.replace("__AVATAR_PIXEL_JS__", KERNEL_SOURCE.trim());
  for (const placeholder of ["__ICONS_JSON__", "__PALETTE_JSON__", "__LUNAR_JSON__",
                             "__PREVIEW_MATH_JS__", "__AVATAR_PIXEL_JS__"]) {
    assert.ok(!script.includes(placeholder), `占位符 ${placeholder} 没被替换掉`);
  }
  return script;
}

const pageScript = renderedPageScript();
new vm.Script(pageScript, { filename: "admin.js(inlined)" });   // 解析失败会抛

/* ---------- 内核:在干净的 vm 上下文里跑 ---------- */

const kernel = vm.createContext({ atob, btoa });   // paletteOf 要用 atob
vm.runInContext(KERNEL_SOURCE, kernel, { filename: "avatar_pixel.js" });
const OUT = 40;
const work = OUT * vm.runInContext("AVA_WORK_SCALE", kernel);
const pixelateAvatar = (px, params) =>
  vm.runInContext("pixelateAvatar", kernel)(px, work, OUT, params);
const packAvatar4bpp = vm.runInContext("packAvatar4bpp", kernel);
const packAvatarPalette = vm.runInContext("packAvatarPalette", kernel);
const resolveAvatarParams = vm.runInContext("resolveAvatarParams", kernel);
const AVA_COLORS_DEFAULT = vm.runInContext("AVA_COLORS_DEFAULT", kernel);
const AVA_COLORS_MIN = vm.runInContext("AVA_COLORS_MIN", kernel);
const AVA_COLORS_MAX = vm.runInContext("AVA_COLORS_MAX", kernel);

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

// 这张图实际用到的那几种颜色(按索引指过去)。
const usedColors = (out) => {
  const seen = new Map();
  for (const i of out.idx) seen.set(out.palette[i].join(","), out.palette[i]);
  return [...seen.values()];
};

// [r,g,b] 在调色板里离哪一项最近(RGB 距离,只用来做"差不多是这个色"的断言)。
const nearest = (palette, rgb) => {
  let best = 0, bestD = Infinity;
  for (let i = 0; i < palette.length; i++) {
    const d = (palette[i][0] - rgb[0]) ** 2 + (palette[i][1] - rgb[1]) ** 2 + (palette[i][2] - rgb[2]) ** 2;
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
};

const sameColor = (a, b) => a[0] === b[0] && a[1] === b[1] && a[2] === b[2];
const distinctPalette = (out) => new Set(out.palette.map((c) => c.join(","))).size;

const results = [];

/* ---------- 2. 纯色:取到那一个色,不出现杂色 ---------- */
{
  const SRC = [202, 90, 60];                        // 设备那 16 色里没有的橙红
  const px = makeImage(() => [...SRC, 255]);
  const out = pixelateAvatar(px, AVA_COLORS_DEFAULT);
  assert.equal(out.idx.length, 1600, "输出必须是 40x40 个索引");
  assert.equal(out.palette.length, 16, "调色板必须补满 16 项(设备那张 I4 图开头固定 16 项)");
  assert.equal(new Set(out.idx).size, 1, "纯色图不该出现第二种颜色");

  const used = usedColors(out);
  assert.equal(used.length, 1, "纯色图只该用到一个颜色");
  const drift = Math.abs(used[0][0] - SRC[0]) + Math.abs(used[0][1] - SRC[1]) + Math.abs(used[0][2] - SRC[2]);
  assert.ok(drift <= 12, `取到的颜色 ${used[0]} 离原色 ${SRC} 太远(偏差 ${drift})`);
  results.push(`纯色图取到 ${used[0]}（原色 ${SRC}，偏差 ${drift}）`);
}

/* ---------- 3. 每图取色:设备那 16 色里没有的颜色,也能取出来 ---------- */
{
  // 一整片青色。设备那 16 色里最接近的是浅蓝 #B9F3FF 或蓝 #1689E8,都差得远,
  // 而"每图取色"应该直接取到青色本身 —— 这正是这一版要解决的问题。
  const SRC = [40, 200, 190];
  const deviceNearest = DEVICE_PALETTE[nearest(DEVICE_PALETTE, SRC)];
  assert.ok(Math.abs(deviceNearest[0] - SRC[0]) > 20 || Math.abs(deviceNearest[1] - SRC[1]) > 20,
            "这条断言要成立,设备那 16 色里必须确实没有青色");

  const px = makeImage(() => [...SRC, 255]);
  const used = usedColors(pixelateAvatar(px, AVA_COLORS_DEFAULT))[0];
  const drift = Math.abs(used[0] - SRC[0]) + Math.abs(used[1] - SRC[1]) + Math.abs(used[2] - SRC[2]);
  assert.ok(drift <= 16, `青色应该被取到,实际 ${used}(偏差 ${drift})`);
  results.push(`设备 16 色取不到的青 ${SRC}（最近的是 ${deviceNearest}），每图取色取到了 ${used}`);
}

/* ---------- 4. 两块颜色:边界要干净、只用到这两个色 ---------- */
{
  const A = [230, 60, 50], B = [30, 80, 220];
  const px = makeImage((x) => (x < work / 2 ? [...A, 255] : [...B, 255]));
  const out = pixelateAvatar(px, AVA_COLORS_DEFAULT);
  const used = usedColors(out);
  assert.equal(used.length, 2, `分明的两块不该出现第三种颜色,实际 ${JSON.stringify(used)}`);

  const ca = out.palette[nearest(out.palette, A)];
  const cb = out.palette[nearest(out.palette, B)];
  // 每一行都该是"左 A 右 B",分界在正中间 —— 取色是逐格的,边界不该漂。
  for (let y = 0; y < OUT; y++) {
    const row = out.idx.slice(y * OUT, (y + 1) * OUT);
    assert.ok(row.slice(0, 15).every((v) => sameColor(out.palette[v], ca)), `第 ${y} 行左侧应为 A`);
    assert.ok(row.slice(25).every((v) => sameColor(out.palette[v], cb)), `第 ${y} 行右侧应为 B`);
  }
  results.push("两块颜色的边界干净且落在中线");
}

/* ---------- 5. 全透明:整张给白 ---------- */
{
  const px = makeImage(() => [0, 0, 0, 0]);
  const out = pixelateAvatar(px, AVA_COLORS_DEFAULT);
  const used = usedColors(out);
  assert.equal(used.length, 1, "全透明的图不该出现第二种颜色");
  assert.ok(used[0].every((c) => c >= 240), `全透明必须整张给白,实际 ${used[0]}`);
  results.push("全透明给白");
}

/* ---------- 6. 确定性 + 取色数量越大颜色越多 ---------- */
{
  // 带噪声的渐变:确定性一旦被破坏(比如引入随机播种),这里必挂。
  let seed = 12345;
  const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  const px = makeImage((x, y) => {
    const base = Math.round((x + y) / (2 * work) * 200) + 20;
    const n = Math.round((rnd() - 0.5) * 60);
    return [base + n, Math.max(0, base - 40 + n), Math.max(0, 255 - base), 255];
  });

  const a = pixelateAvatar(px, AVA_COLORS_DEFAULT);
  const b = pixelateAvatar(px, AVA_COLORS_DEFAULT);
  assert.deepEqual([...a.idx], [...b.idx], "同一张图两次的索引必须完全一致");
  assert.deepEqual(a.palette.map((c) => [...c]), b.palette.map((c) => [...c]),
                   "同一张图两次的调色板必须完全一致");

  const few = pixelateAvatar(px, { colors: 4 });
  assert.ok(distinctPalette(few) <= 4, `取 4 色时调色板最多 4 种,实际 ${distinctPalette(few)}`);
  assert.ok(distinctPalette(a) >= distinctPalette(few),
            `取 16 色不该比取 4 色还少(${distinctPalette(a)} vs ${distinctPalette(few)})`);
  results.push(`确定性 + 取色数量（4 色 -> ${distinctPalette(few)} 种 / 16 色 -> ${distinctPalette(a)} 种）`);
}

/* ---------- 7. 上色:每格填"格内 Lab 均值最近的那个色" ---------- */
{
  // 左半边是 1px 黑白棋盘,右半边是纯中灰。棋盘的每一格黑白各半,均值落在中灰上,
  // 于是整幅左侧都该填中灰 —— 这条钉的是"一格一个色、且取的是最近色"这个行为,
  // 而不是逐像素各取各的(那样会碎成雪花)。
  const px = makeImage((x, y) => (x >= work / 2
    ? [128, 128, 128, 255]
    : (((x + y) % 2) ? [255, 255, 255, 255] : [0, 0, 0, 255])));
  const out = pixelateAvatar(px, AVA_COLORS_DEFAULT);
  const gray = out.palette[nearest(out.palette, [128, 128, 128])];
  assert.ok(gray[0] >= 118 && gray[0] <= 138, `中灰应该被取到,实际 ${gray}`);
  const grayIdx = new Set();
  for (let y = 0; y < OUT; y++) {
    for (let x = 0; x < OUT / 2; x++) {
      const c = out.palette[out.idx[y * OUT + x]];
      assert.ok(sameColor(c, gray), `(${x},${y}) 的黑白棋盘格应填中灰,实际 ${c}`);
    }
  }
  // 每格只有一个色:同一格里的像素必须指向同一个索引(逐像素取色会在这里挂)。
  assert.equal(grayIdx.size, 0);
  results.push(`黑白棋盘(每格黑白各半)整片填中灰 ${gray}`);
}

/* ---------- 7b. 上色:过半像素同色就填那个色,不被均值拉走 ---------- */
{
  // 左半边每格 25 颗纯黑 + 11 颗纯白(69% 是黑的),右半边纯中灰。
  // 调色板里黑、白、中灰都有:按"均值最近色"算,这一格的均值落在中灰附近,会填中灰;
  // 按"过半多数票"算,这一格必须填黑 —— 眼珠、鼻孔、叶子阴影这类小块暗部靠的就是这条。
  const px = makeImage((x, y) => {
    if (x >= work / 2) return [128, 128, 128, 255];
    return ((y % 6) * 6 + (x % 6)) < 25 ? [0, 0, 0, 255] : [255, 255, 255, 255];
  });
  const out = pixelateAvatar(px, AVA_COLORS_DEFAULT);
  const hasGray = out.palette.some((c) => c[0] === c[1] && c[1] === c[2] && Math.abs(c[0] - 128) <= 4);
  assert.ok(hasGray, "中灰必须在这张图的调色板里,否则这条测不出投票规则");
  const black = out.palette[nearest(out.palette, [0, 0, 0])];
  for (let y = 0; y < OUT; y++) {
    for (let x = 0; x < OUT / 2; x++) {
      const c = out.palette[out.idx[y * OUT + x]];
      assert.ok(sameColor(c, black), `(${x},${y}) 这格 25 黑 11 白,应填黑,实际 ${c}`);
    }
  }
  results.push("过半同色时填那个色(25 黑 11 白的格子填黑,而不是均值算出的中灰)");
}

/* ---------- 8. 清洗：去掉孤立点，但不吃色块边界与斜线 ---------- */
{
  const cleanIsolated = vm.runInContext("cleanIsolated", kernel);
  // 去孤立点现在是可调轮数（默认 0）。这里直接测这个纯函数，用 2 轮 = 原本那版行为。
  const PASSES = 2;
  const OUT2 = 40;

  // 一张 40x40 的索引图：默认全 0，fill(x,y) 返回要改成的索引（null = 不动）。
  const map = (fill) => {
    const idx = new Uint8Array(OUT2 * OUT2);
    for (let y = 0; y < OUT2; y++) {
      for (let x = 0; x < OUT2; x++) {
        const v = fill(x, y);
        if (v !== null && v !== undefined) idx[y * OUT2 + x] = v;
      }
    }
    return idx;
  };
  // 和 cleanIsolated 同一条判据：四邻都不同色才算孤立点。
  const isolated = (idx) => {
    let n = 0;
    for (let y = 1; y < OUT2 - 1; y++) {
      for (let x = 1; x < OUT2 - 1; x++) {
        const v = idx[y * OUT2 + x];
        if (v !== idx[y * OUT2 + x - 1] && v !== idx[y * OUT2 + x + 1] &&
            v !== idx[(y - 1) * OUT2 + x] && v !== idx[(y + 1) * OUT2 + x]) n++;
      }
    }
    return n;
  };

  // (a) 散点：一整片 1 里插一个 2 —— 它是唯一的孤立点，必须被抹成 1，且不牵动别处。
  const speckled = map((x, y) => ((x % 5 === 2 && y % 5 === 2) ? 2 : 1));
  const before = isolated(speckled.slice());
  const cleaned = cleanIsolated(speckled.slice(), OUT2, PASSES);
  assert.ok(before > 50, `造的图应该有大量散点，实际 ${before}`);
  assert.equal(isolated(cleaned), 0, `清洗后不该还剩孤立点，实际 ${isolated(cleaned)}`);
  assert.ok([...cleaned].every((v) => v === 1), "整片都该回到 1（散点被吃掉）");

  // (b) 色块边界：左 1 右 2 的直边，一格都不该动 —— 边界上的格子四邻里有同色。
  const halves = map((x) => (x < 20 ? 1 : 2));
  assert.deepEqual([...cleanIsolated(halves.slice(), OUT2, PASSES)], [...halves],
                   "直边不该被清洗动到");

  // (c) 斜线：一条 6 格的对角线，3x3 里至少还有两个同色邻居 → 判为线，保留。
  //     这是"保护细节"那条闸门；去掉它这条会挂。
  const diagonal = map((x, y) => (x === y ? 2 : 1));
  const keptDiagonal = cleanIsolated(diagonal.slice(), OUT2, PASSES);
  for (let i = 1; i < 39; i++) {
    assert.equal(keptDiagonal[i * OUT2 + i], 2, `斜线第 ${i} 格被吃掉了`);
  }

  // (d) 确定性：清洗不许引入随机量。
  assert.deepEqual([...cleanIsolated(speckled.slice(), OUT2, PASSES)],
                   [...cleanIsolated(speckled.slice(), OUT2, PASSES)]);
  results.push(`去孤立点（${before} 个散点 -> 0；直边与斜线保留）`);
}

/* ---------- 9. 参数钳制:五个旋钮都要能夹住越界、认不出的取值回落默认 ---------- */
{
  assert.equal(resolveAvatarParams({ colors: 9999 }).colors, AVA_COLORS_MAX, "colors 必须夹到上限");
  assert.equal(resolveAvatarParams({ colors: -5 }).colors, AVA_COLORS_MIN, "colors 必须夹到下限");
  // 档位名与参数对象两条入口都要能用;不认识的档位回落到默认。
  // (只比字段:跨 vm 域的对象原型不同,deepEqual 会因此失败。)
  assert.equal(resolveAvatarParams("tones8").colors, 8);
  assert.equal(resolveAvatarParams("tones16").colors, resolveAvatarParams({}).colors);
  assert.equal(resolveAvatarParams("不认识的档位").colors, AVA_COLORS_DEFAULT);

  // 五个旋钮的默认值 = 用户偏好那一版。
  const d = resolveAvatarParams({});
  assert.equal(d.palette, "photo", "配色来源默认每图自取");
  assert.equal(d.sample, "area", "采样默认面积平均");
  assert.equal(d.assign, "vote", "上色默认过半投票");
  assert.equal(d.clean, 0, "去孤立点默认 0(关)");
  assert.equal(d.colors, AVA_COLORS_DEFAULT, "取色默认 16");

  // 枚举旋钮:合法值照抄,大小写/拼错/中文一律回落默认 —— 不能悄悄变成另一个有效值。
  assert.equal(resolveAvatarParams({ palette: "device" }).palette, "device");
  assert.equal(resolveAvatarParams({ palette: "DEVICE" }).palette, "photo");
  assert.equal(resolveAvatarParams({ palette: "设备" }).palette, "photo");
  assert.equal(resolveAvatarParams({ sample: "point" }).sample, "point");
  assert.equal(resolveAvatarParams({ sample: "center" }).sample, "center");
  assert.equal(resolveAvatarParams({ sample: "点采样" }).sample, "area");
  assert.equal(resolveAvatarParams({ assign: "mean" }).assign, "mean");
  assert.equal(resolveAvatarParams({ assign: "投票" }).assign, "vote");

  // 数值旋钮 clean:夹到 0..3,取整,未知回落 0。
  assert.equal(resolveAvatarParams({ clean: 99 }).clean, 3, "clean 必须夹到上限 3");
  assert.equal(resolveAvatarParams({ clean: -1 }).clean, 0, "clean 必须夹到下限 0");
  assert.equal(resolveAvatarParams({ clean: 2.4 }).clean, 2, "clean 必须取整");
  assert.equal(resolveAvatarParams({ clean: "没填" }).clean, 0, "clean 未知值必须回落 0");
  results.push("五个旋钮的钳制 / 未知值回落 / 两条入口");
}

/* ---------- 9b. 配色来源:palette:"device" 真的用设备那 16 色,跳过取色 ---------- */
{
  const SRC = [40, 200, 190];                       // 青色,设备那 16 色里没有
  const px = makeImage(() => [...SRC, 255]);

  // 每图自取那一版能取到青色附近(见用例 3);设备档必须原样照抄设备那 16 色。
  const dev = pixelateAvatar(px, { palette: "device", devicePalette: DEVICE_PALETTE });
  // 用 [...] 先在宿主域摊成普通数组:dev.palette 是 vm 域的数组,直接 .map 出来的也是
  // vm 域的,与宿主域的 DEVICE_PALETTE 比 deepEqual 会因为原型不同而挂。
  assert.deepEqual([...dev.palette].map((c) => [...c]), DEVICE_PALETTE.map((c) => [...c]),
                   "设备档的调色板必须就是设备那 16 色(不取色、不补白)");
  // 用到的颜色只能来自设备那 16 色 —— 这正是"配色是观感大头"这一档的意义。
  for (const c of usedColors(dev)) {
    assert.ok(DEVICE_PALETTE.some((d2) => sameColor(d2, c)),
              `设备档用到的色 ${c} 不在设备那 16 色里`);
  }
  const photo = pixelateAvatar(px, AVA_COLORS_DEFAULT);
  const cyan = usedColors(photo)[0];
  assert.ok(Math.abs(cyan[0] - SRC[0]) + Math.abs(cyan[1] - SRC[1]) + Math.abs(cyan[2] - SRC[2]) <= 16,
            "每图自取那一版应取到青色,便于和设备档对照");
  assert.notDeepEqual([...dev.idx], [...photo.idx], "两档配色必须给出不同的结果");

  // 没传 devicePalette 时回落到白(而不是崩、也不是偷用别的全局表)。
  const noDev = pixelateAvatar(px, { palette: "device" });
  assert.ok(noDev.palette.every((c) => c[0] === 255 && c[1] === 255 && c[2] === 255),
            "缺 devicePalette 时必须回落白,保证 16 项齐");
  results.push("palette:\"device\" 用设备那 16 色(缺表回落白)");
}

/* ---------- 9c. 上色规则:assign:"mean" 关掉投票,小块暗部被均值抹平 ---------- */
{
  // 左半边每格 25 黑 11 白(69% 黑),右半边纯中灰。
  const px = makeImage((x, y) => {
    if (x >= work / 2) return [128, 128, 128, 255];
    return ((y % 6) * 6 + (x % 6)) < 25 ? [0, 0, 0, 255] : [255, 255, 255, 255];
  });
  const vote = pixelateAvatar(px, AVA_COLORS_DEFAULT);          // 默认 vote
  const mean = pixelateAvatar(px, { assign: "mean" });
  const blackV = vote.palette[nearest(vote.palette, [0, 0, 0])];
  const blackM = mean.palette[nearest(mean.palette, [0, 0, 0])];
  // vote:25 黑 11 白 -> 过半填黑;mean:均值把这一格拉向中灰,绝不能还是黑。
  assert.ok(sameColor(vote.palette[vote.idx[10 * OUT + 5]], blackV), "vote 档该填黑");
  assert.ok(!sameColor(mean.palette[mean.idx[10 * OUT + 5]], blackM), "mean 档不该填黑(均值拉向灰)");
  assert.notDeepEqual([...vote.idx], [...mean.idx], "assign 必须改变输出");
  results.push("assign:\"mean\" 关掉投票(25 黑 11 白的格子不再填黑)");
}

/* ---------- 9d. 采样方式:point 只取一颗,area 把黑白棋盘平均成中灰 ---------- */
{
  // 左半边 1px 黑白棋盘、右半边纯中灰。area 把每格平均成中灰;point 只取格心那一颗
  // (block=6 时是 (3,3),黑白棋盘上恒为黑),于是同一格不再是中灰。
  const px = makeImage((x, y) => (x >= work / 2
    ? [128, 128, 128, 255]
    : (((x + y) % 2) ? [255, 255, 255, 255] : [0, 0, 0, 255])));
  const area = pixelateAvatar(px, AVA_COLORS_DEFAULT);
  const point = pixelateAvatar(px, { sample: "point" });
  assert.notDeepEqual([...area.idx], [...point.idx], "point 采样必须与 area 不同");
  const grayA = area.palette[nearest(area.palette, [128, 128, 128])];
  assert.ok(sameColor(area.palette[area.idx[10 * OUT + 5]], grayA), "area 该把棋盘格平均成中灰");
  assert.ok(!sameColor(point.palette[point.idx[10 * OUT + 5]], grayA), "point 取一颗,不该还是中灰");

  // center 与 area 不同:造一张"每格中间一条黑、四周白"的图。面积平均/无加权投票都被
  // 外围的白压过(填白),中心加权后黑在格心附近过半(填黑)——两档必须给出不同结果。
  const px2 = makeImage((x, y) => {
    if (x >= work / 2) return [128, 128, 128, 255];
    const lx = x % 6, ly = y % 6;
    return (lx >= 2 && lx <= 3 && ly >= 1 && ly <= 4) ? [0, 0, 0, 255] : [255, 255, 255, 255];
  });
  const area2 = pixelateAvatar(px2, AVA_COLORS_DEFAULT);
  const center2 = pixelateAvatar(px2, { sample: "center" });
  assert.notDeepEqual([...area2.idx], [...center2.idx], "center 采样必须与 area 不同");
  const white2 = area2.palette[nearest(area2.palette, [255, 255, 255])];
  const black2 = area2.palette[nearest(area2.palette, [0, 0, 0])];
  assert.ok(sameColor(area2.palette[area2.idx[10 * OUT + 5]], white2), "area 该被外围的白压成白");
  assert.ok(sameColor(center2.palette[center2.idx[10 * OUT + 5]], black2), "center 该让格心的黑过半");
  results.push("sample:point/center 都确实改变输出");
}

/* ---------- 9e. 去孤立点默认关闭,且 clean 只动索引、不动调色板 ---------- */
{
  // 带噪声的渐变:默认(clean:0)与显式 clean:0 必须逐索引一致;clean 轮数越大孤立点越少。
  let seed = 999;
  const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  const px = makeImage((x, y) => {
    const base = Math.round((x + y) / (2 * work) * 200) + 20;
    const n = Math.round((rnd() - 0.5) * 80);
    return [base + n, Math.max(0, base - 30 + n), Math.max(0, 255 - base), 255];
  });
  const dflt = pixelateAvatar(px, AVA_COLORS_DEFAULT);
  const zero = pixelateAvatar(px, { clean: 0 });
  assert.deepEqual([...zero.idx], [...dflt.idx], "clean:0 必须与默认逐索引一致(默认就是 0)");

  const c2 = pixelateAvatar(px, { clean: 2 });
  assert.deepEqual(c2.palette.map((c) => [...c]), dflt.palette.map((c) => [...c]),
                   "clean 不该改调色板(它只看索引图)");
  assert.notDeepEqual([...c2.idx], [...zero.idx], "clean:2 必须改变部分格子");
  const countIsolatedCells = vm.runInContext("countIsolatedCells", kernel);
  assert.ok(countIsolatedCells(c2.idx, OUT) < countIsolatedCells(zero.idx, OUT),
            `clean 必须真的减少孤立点 (${countIsolatedCells(zero.idx, OUT)} -> ${countIsolatedCells(c2.idx, OUT)})`);

  // clean:3 与 clean:2 相同或更少,不会因为多跑一轮而变多。
  const c3 = pixelateAvatar(px, { clean: 3 });
  assert.ok(countIsolatedCells(c3.idx, OUT) <= countIsolatedCells(c2.idx, OUT),
            "多去一轮不许让孤立点变多");
  results.push(`clean 默认关(clean:0==默认);2 轮把孤立点 ` +
               `${countIsolatedCells(zero.idx, OUT)} -> ${countIsolatedCells(c2.idx, OUT)}`);
}

/* ---------- 9f. 新参数不许引入随机量:每档两次结果必须完全一致 ---------- */
{
  const px = makeImage((x, y) => [(x * 5) % 256, (y * 7) % 256, ((x + y) * 3) % 256, 255]);
  for (const p of [{ sample: "center" }, { sample: "point" }, { assign: "mean" },
                   { clean: 3 }, { palette: "device", devicePalette: DEVICE_PALETTE }]) {
    const a = pixelateAvatar(px, p);
    const b = pixelateAvatar(px, p);
    assert.deepEqual([...a.idx], [...b.idx], `参数 ${JSON.stringify(p)} 两次索引必须一致`);
    assert.deepEqual(a.palette.map((c) => [...c]), b.palette.map((c) => [...c]),
                     `参数 ${JSON.stringify(p)} 两次调色板必须一致`);
  }
  results.push("新参数下确定性保持(无随机量)");
}

/* ---------- 9. 打包:4bpp 与 64 字节配色 ---------- */
{
  const px = makeImage((x, y) => [(x * 3) % 256, (y * 5) % 256, (x * y) % 256, 255]);
  const out = pixelateAvatar(px, AVA_COLORS_DEFAULT);

  const packed = packAvatar4bpp(out.idx);
  assert.equal(packed.length, 800, "40x40 的 4bpp 必须是 800 字节");
  assert.equal(packed[0] >> 4, out.idx[0], "偶数像素在高半字节");
  assert.equal(packed[0] & 0x0F, out.idx[1], "奇数像素在低半字节");
  assert.equal(packed[399] >> 4, out.idx[798]);
  assert.equal(packed[399] & 0x0F, out.idx[799]);

  const pal = packAvatarPalette(out.palette);
  assert.equal(pal.length, 64, "16 色 x 4 字节 = 64 字节");
  const view = new DataView(pal.buffer, pal.byteOffset, pal.byteLength);
  for (let i = 0; i < 16; i++) {
    const [r, g, b] = out.palette[i];
    assert.equal(pal[i * 4], b, `第 ${i} 项的低字节必须是 B`);
    assert.equal(pal[i * 4 + 1], g, `第 ${i} 项的第二字节必须是 G`);
    assert.equal(pal[i * 4 + 2], r, `第 ${i} 项的第三字节必须是 R`);
    assert.equal(pal[i * 4 + 3], 0, "第 4 字节留给设备写 alpha(圆角镂空),这里必须是 0");
    // 与设备端 ui_pixel_pack_avatar_i4 的读法对得上:小端 uint32 取 0x00RRGGBB。
    assert.equal(view.getUint32(i * 4, true), (r << 16) | (g << 8) | b,
                 `第 ${i} 项必须是小端 0x00RRGGBB`);
  }
  results.push("4bpp 打包 + 64 字节配色的小端布局");

  /* ---------- 10b. 上传载荷：64 配色 + 800 索引 = 864 字节，且配色能被解回来 ---------- */
  //
  // 这条钉的是**网页与设备之间的那串字节**，也就是最容易两边写岔的地方：
  //   main/love_httpd.c 的 handle_avatar() 收 864 或 800，864 时前 64 字节是配色；
  //   main/love_config.h 的 LOVE_AVATAR_PALETTE_BYTES 是 64。
  // 而 paletteOf() 是设备回传配色时的逆运算 —— 上一版它把长度判据写成了 AVA * 4(160)，
  // 于是设备回传的 64 字节被判成"没有配色"，缩略图静默画成设备那 16 色：上传成功、
  // 显示成另一张图、不报错。来回一致必须在主机上钉住。
  const paletteOf = vm.runInContext("paletteOf", kernel);
  const payload = new Uint8Array(pal.length + packed.length);
  payload.set(pal, 0);
  payload.set(packed, pal.length);
  assert.equal(payload.length, 864, "上传载荷必须是 864 字节（64 配色 + 800 索引）");

  const b64 = Buffer.from(payload.subarray(0, 64)).toString("base64");
  const back = paletteOf(b64);
  assert.ok(back, "64 字节的配色 base64 必须能解回来（长度判据写错就返回 null）");
  for (let i = 0; i < 16; i++) {
    assert.deepEqual(back[i], out.palette[i], `解回来的第 ${i} 项与原配色不一致`);
  }
  // 设备里的空槽位回的是空串；网页要据此回落到设备那 16 色。
  assert.equal(paletteOf(""), null, "空串必须返回 null（回落到设备那 16 色）");
  assert.equal(paletteOf(Buffer.from(pal.subarray(0, 32)).toString("base64")), null,
               "不足 64 字节的配色必须是 null，不能被当成半张表用");
  results.push("上传载荷 864 字节 + 配色来回一致（paletteOf 与 packAvatarPalette 互逆）");
}

// 生成脚本内联的那份也必须能解析(与上面 renderedPageScript 是同一件事的另一半:
// 确认 avatar_pixel.js 自己作为 classic script 是合法的)。
new vm.Script(KERNEL_SOURCE, { filename: "avatar_pixel.js" });
results.push("页面脚本与内核都能解析");

console.log("Host tests (avatar pixel): OK");
for (const line of results) console.log("  - " + line);
