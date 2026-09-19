// assets/web/avatar_pixel.js —— 头像的像素化内核：每图自带调色板 + 逐格选色。
//
// 单独成文件是为了**能被主机测试直接跑**：tests/test_avatar_pixel.mjs 用 node 的 vm
// 加载它、喂合成图、对输出做断言。页面侧由 tools/gen_admin_page.py 把整份文件内联
// 进 admin.js（admin.js 里那一行占位符），所以这里不能有 import/export，
// **也不能定义页面已经有的名字**（AVA / PALETTE / AVATAR_BYTES 都在 admin.js 里），
// 尺寸一律由调用方当参数传进来。
//
// 管线（照 pixeltool.art 的 pixelArt.worker 重写，见下）：
//   240x240 工作图 → 5 位直方图 → 中位切分取 N 色 → Lloyd 迭代精修
//   → 用真实像素求出每个色 → 每格按"格内多数票"取色 → 去孤立点
//   → 40x40 索引 + 16 个 0x00RRGGBB
//
// 与上一版（SLIC 超像素 + 设备那 16 色固定调色板）的区别，是拿照片实测出来的：
//
//   1. **配色是观感的大头，不是降采样算法。** 设备那 16 色是给图标配的卡通色，
//      几乎没有中间调：深肤色会被映射成 #17202A 的深蓝，灰帽子会变成灰底黑噪点。
//      让每张照片自己取 16 色，同一批测试图都从"卡通贴纸"变成"认得出是谁的像素人像"。
//      代价是设备要多存 64 字节，见 main/love_store.c。
//   2. **不需要聚类出"区域"。** 上一版用 SLIC 把画面切成一片片区域、再让整片共用
//      一个色，结果是块状伪影；按输出格子直接取色反而更干净，也快得多 —— 40x40 上
//      本来就一格一个色，区域比格子还小的时候"投票"退化成逐像素取色。
//   3. **"去孤立点"是一个取舍，不是定理。** 用 16 色量化一块连续渐变，四邻都不同色的
//      散点会占到 6% 的格子；把其中真散点抹掉能清掉一层脏点，但那些格子里也**混着真
//      细节**（实测孤立点 92→12 的同时 ΔE 5.4→6.0）。这个交换归用户拍板，所以它现在是
//      可调的、**默认 0（不清洗）** —— 见 cleanIsolated()。
//
// 参考实现（pixeltool.art）用的是点采样降采样：在他们的输出宽度（64~256）上不噪，在
// 我们的 40x40 上会碎成雪花，所以**不是**我们的默认。但"细节多少"正是用户要自己拿捏的
// 东西，于是把它做成 sample:"point" 一档。有序抖动仍然不提供：40x40 上抖动整张都是
// 网点，试过，见 assets/README.md。

// 工作分辨率相对输出格子的倍数：6 表示每个输出像素覆盖 6x6 个源像素（40 -> 240）。
// 直接在 40x40 上取色和选色都没有意义：一格只有一颗源像素，"选色"退化成取最近色。
const AVA_WORK_SCALE = 6;

// 取色数量：越多越接近原图（也越"柔"，因为相邻格子的颜色差变小），越少越像
// 像素画（大片平涂）。16 是上限 —— 设备一张 I4 图只有 16 个调色板项。
const AVA_COLORS_MIN = 4;
const AVA_COLORS_MAX = 16;
const AVA_COLORS_DEFAULT = 16;

// 旧入口：档位名直接映射成取色数量（主机测试与老调用点还在用）。页面上的三档预设
// 已经升级成"一整组参数"（见 admin.js 的 AVATAR_PRESETS），这里保留字符串入口只为兼容。
const AVA_PRESETS = { tones16: 16, tones12: 12, tones8: 8 };
const AVA_PRESET_DEFAULT = "tones16";

// 配色来源：
//   photo  = 每图自取（默认，观感最好，见上）；
//   device = 用设备那 16 色图标配色，**跳过整个取色阶段**。
// 留 device 这一档，是为了让用户能直接对照出"配色才是观感的大头" —— 同一张照片切过去
// 会立刻退回"卡通贴纸"的样子（实测 ΔE 5.4→18.6）。这 16 色不在内核里定义，由调用方
// 通过 params.devicePalette 传进来（页面传自己的 PALETTE 常量，测试传 assets.json）
// ——内核里不能出现 PALETTE 这个名字，admin.js 已经有了。
const AVA_PALETTE_SOURCES = ["photo", "device"];
const AVA_PALETTE_DEFAULT = "photo";

// 每格 36 个源像素怎么变成一个代表色 —— 这是"细节多少"最直接的旋钮：
//   area   = 面积平均（默认，就是当前这样）：最稳，细节最少，也最不容易出雪花；
//   center = 中心加权平均（二维高斯，离格心越近权重越大）：比 area 多一点主体细节；
//   point  = 最近邻点采样（只取格心那一颗）：细节最多，也最容易碎成雪花 ——
//            pixeltool.art 就是这么干的，但他们的输出有 64~256 宽。
const AVA_SAMPLE_MODES = ["area", "center", "point"];
const AVA_SAMPLE_DEFAULT = "area";

// 上色规则：
//   vote = 格内像素各自投给最近的调色板色，过半就填它、没有过半回落到格内 Lab 均值
//          （默认，见 drawAvatarIndices 的说明）；
//   mean = 一律用格内（加权）均值。更"平滑"，代价是把小块暗部（眼珠、鼻孔）一并抹平。
const AVA_ASSIGN_MODES = ["vote", "mean"];
const AVA_ASSIGN_DEFAULT = "vote";

// 去孤立点轮数。默认 0 = 完全不动：这一步是取舍不是定理，交给用户；上限 3 是因为
// 实测第 4 轮起输出不再变化（能清的早清完了），多几轮只是白算。
const AVA_CLEAN_MAX_PASSES = 3;
const AVA_CLEAN_DEFAULT_PASSES = 0;

// 5 位 cube 的键（r>>3<<10 | g>>3<<5 | b>>3）与它代表的 8 位颜色（取格心）。
const AVA_CUBE = 32768;
const cubeKey = (r, g, b) => (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3);
const cubeR = (k) => (((k >> 10) & 31) * 255 + 15) >> 5;
const cubeG = (k) => (((k >> 5) & 31) * 255 + 15) >> 5;
const cubeB = (k) => ((k & 31) * 255 + 15) >> 5;

// sRGB -> 线性的查找表。Lab 转换里那两次幂运算占了整张图的大头，查表省掉其中最贵的一次。
const AVA_SRGB_LINEAR = (() => {
  const t = new Float32Array(256);
  for (let i = 0; i < 256; i++) {
    const c = i / 255;
    t[i] = c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
  }
  return t;
})();

// sRGB -> CIELAB(D65)。写进并排的数组而不是返回对象：几万个像素每像素造一个
// 对象既慢又费内存。
function rgb2labTo(r, g, b, L, A, B, at) {
  const R = AVA_SRGB_LINEAR[r], G = AVA_SRGB_LINEAR[g], Bl = AVA_SRGB_LINEAR[b];
  const x = (R * 0.4124564 + G * 0.3575761 + Bl * 0.1804375) / 0.950456;
  const y = R * 0.2126729 + G * 0.7151522 + Bl * 0.0721750;
  const z = (R * 0.0193339 + G * 0.1191920 + Bl * 0.9503041) / 1.088754;
  const fx = x > 0.008856 ? Math.cbrt(x) : 7.787 * x + 16 / 116;
  const fy = y > 0.008856 ? Math.cbrt(y) : 7.787 * y + 16 / 116;
  const fz = z > 0.008856 ? Math.cbrt(z) : 7.787 * z + 16 / 116;
  L[at] = 116 * fy - 16;
  A[at] = 500 * (fx - fy);
  B[at] = 200 * (fy - fz);
}

// CIELAB(D65) -> sRGB。Lloyd 迭代每轮都要把簇心搬回像素空间，所以这次逆变换是必需的。
function lab2rgb(L, A, B) {
  const fy = (L + 16) / 116, fx = fy + A / 500, fz = fy - B / 200;
  const inv = (t) => { const t3 = t * t * t; return t3 > 0.008856 ? t3 : (116 * t - 16) / 903.3; };
  const x = inv(fx) * 0.950456, y = inv(fy), z = inv(fz) * 1.088754;
  const R = x * 3.2404542 + y * -1.5371385 + z * -0.4985314;
  const G = x * -0.9692660 + y * 1.8760108 + z * 0.0415560;
  const Bl = x * 0.0556434 + y * -0.2040259 + z * 1.0572252;
  const enc = (c) => {
    const v = c <= 0.0031308 ? 12.92 * c : 1.055 * Math.pow(c, 1 / 2.4) - 0.055;
    return Math.max(0, Math.min(255, Math.round(v * 255)));
  };
  return [enc(R), enc(G), enc(Bl)];
}

function labDist(l1, a1, b1, l2, a2, b2) {
  const dl = l1 - l2, da = a1 - a2, db = b1 - b2;
  return dl * dl + da * da + db * db;
}

// 把档位名或半成品参数补全成一组可用参数（页面上的高级菜单直接调这个数）。
// 五个旋钮**全部在这里钳制**：数值型越界夹到边界，枚举型拼错/缺失回落到默认 ——
// 都用白名单判断，不写"等于某个值时特殊处理"，否则拼错的取值会悄悄变成另一个有效值。
// devicePalette 原样带过（只有在 palette:"device" 时才用得到）。
function resolveAvatarParams(params) {
  const base = typeof params === "string" ? { colors: AVA_PRESETS[params] } : (params || {});
  const n = Number(base.colors);
  const colors = Number.isFinite(n)
    ? Math.min(AVA_COLORS_MAX, Math.max(AVA_COLORS_MIN, Math.round(n)))
    : AVA_COLORS_DEFAULT;
  const palette = AVA_PALETTE_SOURCES.indexOf(base.palette) >= 0 ? base.palette : AVA_PALETTE_DEFAULT;
  const sample = AVA_SAMPLE_MODES.indexOf(base.sample) >= 0 ? base.sample : AVA_SAMPLE_DEFAULT;
  const assign = AVA_ASSIGN_MODES.indexOf(base.assign) >= 0 ? base.assign : AVA_ASSIGN_DEFAULT;
  const m = Number(base.clean);
  const clean = Number.isFinite(m)
    ? Math.min(AVA_CLEAN_MAX_PASSES, Math.max(0, Math.round(m)))
    : AVA_CLEAN_DEFAULT_PASSES;
  return { colors: colors, palette: palette, sample: sample, assign: assign, clean: clean,
           devicePalette: base.devicePalette };
}

/* ---------- 一张图共用的中间量 ---------- */

// 5 位 cube 直方图（32768 项，只统计不透明的像素）。
// 用定长数组而不是 Map：几万个像素的哈希表在浏览器里明显更慢。
function avatarHistogram(px, work) {
  const hist = new Uint32Array(AVA_CUBE);
  const n = work * work;
  for (let i = 0; i < n; i++) {
    const o = i * 4;
    if (px[o + 3] < 128) continue;   // 近乎透明的像素不参与取色（与上色阶段同一条规则）
    hist[cubeKey(px[o], px[o + 1], px[o + 2])]++;
  }
  return hist;
}

// 每个 5 位格子的 Lab **只算一次**。整条管线里它要用三回（取色、归类、上色），
// 而一张 240x240 的图有几万颗像素、每个像素三次 cbrt —— 这是整个内核最贵的一步，
// 也是早先版本 300ms 里的 200ms。做法是建一张"键 -> 紧凑下标"的表，用到的格子
// （实测一张照片只有 1~2 千个）才进 Lab。
function avatarFeatures(px, work) {
  const hist = avatarHistogram(px, work);
  const entries = [];
  for (let k = 0; k < AVA_CUBE; k++) {
    if (hist[k]) entries.push({ key: k, count: hist[k] });
  }

  const index = new Int32Array(AVA_CUBE).fill(-1);
  const L = new Float32Array(entries.length);
  const A = new Float32Array(entries.length);
  const B = new Float32Array(entries.length);
  for (let i = 0; i < entries.length; i++) {
    const k = entries[i].key;
    index[k] = i;
    rgb2labTo(cubeR(k), cubeG(k), cubeB(k), L, A, B, i);
  }
  return { entries: entries, index: index, L: L, A: A, B: B };
}

/* ---------- 取色 ---------- */

// 中位切分（与 pixeltool.art 的 q() 同一条路子）：
// 反复挑"最长的那个通道跨得最开"的盒子，按加权中位数劈成两半，直到盒子数够。
function medianCut(entries, colors) {
  const bucketOf = (list) => {
    let rmin = 255, rmax = 0, gmin = 255, gmax = 0, bmin = 255, bmax = 0;
    for (const e of list) {
      const r = cubeR(e.key), g = cubeG(e.key), b = cubeB(e.key);
      if (r < rmin) rmin = r; if (r > rmax) rmax = r;
      if (g < gmin) gmin = g; if (g > gmax) gmax = g;
      if (b < bmin) bmin = b; if (b > bmax) bmax = b;
    }
    const dr = rmax - rmin, dg = gmax - gmin, db = bmax - bmin;
    return { entries: list, channel: dr >= dg && dr >= db ? 0 : (dg >= db ? 1 : 2),
             range: Math.max(dr, dg, db) };
  };
  const channelOf = (key, channel) =>
    channel === 0 ? cubeR(key) : (channel === 1 ? cubeG(key) : cubeB(key));

  if (entries.length <= colors) {
    return entries.map((e) => [cubeR(e.key), cubeG(e.key), cubeB(e.key)]);
  }

  const buckets = [bucketOf(entries)];
  while (buckets.length < colors) {
    let pick = -1, widest = -1;
    for (let i = 0; i < buckets.length; i++) {
      // 只挑"劈得动"的盒子：只有一个颜色的盒子再劈也是原地踏步。
      if (buckets[i].range > widest && buckets[i].entries.length > 1) {
        widest = buckets[i].range; pick = i;
      }
    }
    if (pick < 0) break;

    const bucket = buckets[pick];
    const sorted = bucket.entries.slice().sort((a, b) =>
      channelOf(a.key, bucket.channel) - channelOf(b.key, bucket.channel)
      || a.key - b.key);                      // 同值时按键排序：结果必须与输入顺序无关
    const total = sorted.reduce((n, e) => n + e.count, 0);
    let acc = 0, at = 0;
    for (let i = 0; i < sorted.length; i++) {
      acc += sorted[i].count;
      if (acc * 2 >= total) { at = i; break; }
    }
    if (at === 0) at = 1;                      // 保证两个盒子都非空
    buckets.splice(pick, 1, bucketOf(sorted.slice(0, at)), bucketOf(sorted.slice(at)));
  }

  return buckets.map((bucket) => {
    let r = 0, g = 0, b = 0, n = 0;
    for (const e of bucket.entries) {
      r += cubeR(e.key) * e.count; g += cubeG(e.key) * e.count;
      b += cubeB(e.key) * e.count; n += e.count;
    }
    return n ? [Math.round(r / n), Math.round(g / n), Math.round(b / n)] : [0, 0, 0];
  });
}

// Lloyd 迭代（k-means）把中位切分出来的色心再挪几轮。
// 中位切分按"面积"分配颜色：占了大半画面的背景会吃掉很多项，主体反而只剩两三个色。
// 迭代按每个颜色实际覆盖多少像素重新求均值，同样的项数下颜色更贴主体。
// 全在 Lab 里算，且不含任何随机量 —— 同一张图两次结果必须完全一致（有主机测试钉着）。
//
// 返回的是**聚类用的色心**（Lab 三个数组），不是最终颜色：直方图是 5 位 cube，
// 拿格子中心当颜色会有 ±4 的偏差（纯白 255 会变成 247），最后要再扫一遍原图求真实均值。
function refinePalette(features, palette, iters) {
  const entries = features.entries;
  const count = palette.length;
  const n = entries.length;

  const cl = new Float32Array(count), ca = new Float32Array(count), cb = new Float32Array(count);
  for (let i = 0; i < count; i++) {
    rgb2labTo(palette[i][0], palette[i][1], palette[i][2], cl, ca, cb, i);
  }

  const sumL = new Float64Array(count), sumA = new Float64Array(count), sumB = new Float64Array(count);
  const sumW = new Float64Array(count);
  for (let it = 0; it < iters; it++) {
    sumL.fill(0); sumA.fill(0); sumB.fill(0); sumW.fill(0);
    for (let i = 0; i < n; i++) {
      const l = features.L[i], a = features.A[i], b = features.B[i], w = entries[i].count;
      let best = 0, bestD = Infinity;
      for (let c = 0; c < count; c++) {
        const d = labDist(l, a, b, cl[c], ca[c], cb[c]);
        if (d < bestD) { bestD = d; best = c; }
      }
      sumL[best] += l * w; sumA[best] += a * w; sumB[best] += b * w; sumW[best] += w;
    }
    for (let c = 0; c < count; c++) {
      if (!sumW[c]) continue;               // 空簇保留上一轮的中心，别让它漂走
      cl[c] = sumL[c] / sumW[c]; ca[c] = sumA[c] / sumW[c]; cb[c] = sumB[c] / sumW[c];
    }
  }
  return { L: cl, A: ca, B: cb, count: count };
}

// 每个 5 位格子归到哪个簇（只算用到的那些格子，再摊到键上供逐像素查）。
function ownerOfCube(centers, features) {
  const owner = new Uint8Array(AVA_CUBE);
  for (let i = 0; i < features.entries.length; i++) {
    const l = features.L[i], a = features.A[i], b = features.B[i];
    let best = 0, bestD = Infinity;
    for (let c = 0; c < centers.count; c++) {
      const d = labDist(l, a, b, centers.L[c], centers.A[c], centers.B[c]);
      if (d < bestD) { bestD = d; best = c; }
    }
    owner[features.entries[i].key] = best;
  }
  return owner;
}

// 每个簇的最终颜色 = 该簇里**真实像素**的 8 位均值。
// 不用 5 位格子中心，是因为它最高只到 247：纯白、纯色块都会被压掉一档，
// 而"每图取色"的意义正在于颜色要准。没分到像素的簇退回它的 Lab 色心。
function exactClusterPalette(px, work, centers, features) {
  const owner = ownerOfCube(centers, features);
  const sumR = new Float64Array(centers.count), sumG = new Float64Array(centers.count);
  const sumB = new Float64Array(centers.count), sumN = new Float64Array(centers.count);
  const n = work * work;
  for (let i = 0; i < n; i++) {
    const o = i * 4;
    if (px[o + 3] < 128) continue;
    const c = owner[cubeKey(px[o], px[o + 1], px[o + 2])];
    sumR[c] += px[o]; sumG[c] += px[o + 1]; sumB[c] += px[o + 2]; sumN[c]++;
  }
  const out = [];
  for (let c = 0; c < centers.count; c++) {
    out.push(sumN[c]
      ? [Math.round(sumR[c] / sumN[c]), Math.round(sumG[c] / sumN[c]), Math.round(sumB[c] / sumN[c])]
      : lab2rgb(centers.L[c], centers.A[c], centers.B[c]));
  }
  return out;
}

// 不足 16 色时补到 16：设备那张 I4 图的数据开头固定是 16 个调色板项，
// 少一项整张图就错位。补的色只是用不到，不影响画面。
function padPalette(palette) {
  const out = [];
  for (let i = 0; i < AVA_COLORS_MAX; i++) {
    out.push(palette[i] || palette[palette.length - 1] || [255, 255, 255]);
  }
  return out;
}

// 设备那 16 色图标配色：由调用方通过 params.devicePalette 传进来（页面传自己的 PALETTE
// 常量，主机测试传 assets.json 的 palette）。内核里**不定义**这个数组、也不引用任何全局
// 名字 —— admin.js 已经有 PALETTE 了，同名会在这里炸。缺项/非法项一律回落白，保证正好
// 16 项：设备那张 I4 图的数据开头固定 16 项，少一项整张图就错位。
function devicePaletteOf(devicePalette) {
  const out = [];
  for (let i = 0; i < AVA_COLORS_MAX; i++) {
    const c = devicePalette && devicePalette[i];
    out.push(Array.isArray(c) && c.length >= 3
      ? [c[0] & 255, c[1] & 255, c[2] & 255]
      : [255, 255, 255]);
  }
  return out;
}

// 取色：工作图 -> 16 色调色板。
//   palette:"photo"  → 每图自取（中位切分 + Lloyd + 真实像素求均值）；
//   palette:"device" → 直接用调用方给的设备那 16 色，整个取色阶段都跳过。
// opt 缺省时按"每图自取"处理，方便只想要一张调色板的调用点（比如测量脚本）。
function buildAvatarPalette(px, work, colors, features, opt) {
  if (opt && opt.palette === "device") return devicePaletteOf(opt.devicePalette);
  if (!features.entries.length) return padPalette([[255, 255, 255]]);   // 全透明：整张给白
  const cut = medianCut(features.entries, colors);
  const centers = refinePalette(features, cut, 3);
  return padPalette(exactClusterPalette(px, work, centers, features));
}

/* ---------- 清洗：去掉孤立点 ---------- */

// 40x40 只有 1600 格、每图 16 色，一块连续渐变（脸颊、羽毛、背景墙面）必然会被切成
// 一层层的色阶。相邻格落在不同色阶上是正常的"阶梯"，但一格跟**上下左右四邻都不同色**，
// 在 40px 的真机上看就是一颗脏点 —— 实测占 6% 的格子（83/1444）。缩略图放大看还好，
// 真机 1:1 看就是一层噪点。
//
// 规则刻意做得很窄，三道闸门都过了才动手：
//   1. 四邻里有一个同色的，留着（它属于某条线或某个块）；
//   2. 自己在 3x3 里出现超过 AVA_CLEAN_MAX_OWN 次的，留着 —— 斜线上每个格子都跟四邻
//      不同色，但斜线本身是合法笔触。阈值 2 保住的是 3 格以上的斜线；一对角的 2 格
//      "短线"在这个分辨率上与人眼看到的散点没有区别，按噪声处理；
//   3. 3x3 里得有一个"非自己"的色占到 AVA_CLEAN_MIN 格以上，才换成它。
// 于是大面积色块内部不动、边界不会被磨圆，只有真正的散点被吃掉。两轮就够：第一轮
// 清掉散点，第二轮清掉第一轮之后才暴露出来的。
//
// 实测（两张测试照片）：孤立点 92 -> 12、80 -> 10，用到的颜色数一格不少（16/16）。
// 代价是"每格贴近该格均值"这条指标略微变差（ΔE 5.4 -> 6.0）—— 清洗本来就是在用一点点
// 均值误差换掉孤立点。这个交换值不值，是用户要拍的板：所以轮数现在是旋钮（0~3），
// **默认 0**（不交换）。2 轮就是原本那版行为，对应页面"均衡"预设里的那一档。
const AVA_CLEAN_MIN = 3;
// 中心色自己在 3x3 里出现几次以上就判定为"线"而不是"点"。阈值是量出来的：放到 1
// 会有 87% 的散点清不掉（92 -> 45），取消保护则连 3 格以上的斜线一并吃掉（92 -> 5）。
const AVA_CLEAN_MAX_OWN = 2;

function cleanIsolated(idx, outSize, passes) {
  let cur = idx;
  for (let p = 0; p < passes; p++) {
    const next = new Uint8Array(cur);
    for (let y = 1; y < outSize - 1; y++) {
      for (let x = 1; x < outSize - 1; x++) {
        const i = y * outSize + x;
        const own = cur[i];
        // 四邻里有一个同色，它就不是孤立点，留着。
        if (cur[i - 1] === own || cur[i + 1] === own ||
            cur[i - outSize] === own || cur[i + outSize] === own) continue;

        const counts = new Map();
        for (let dy = -1; dy <= 1; dy++) {
          const row = (y + dy) * outSize;
          for (let dx = -1; dx <= 1; dx++) {
            const k = cur[row + x + dx];
            counts.set(k, (counts.get(k) || 0) + 1);
          }
        }
        // 自己在 3x3 里出现太多次 → 它是某条斜线的一段（斜线上每个格子都跟四邻不同色），
        // 不是散点，留着。细斜线在像素画里是合法的笔触，不该被清洗吃掉。
        if ((counts.get(own) || 0) > AVA_CLEAN_MAX_OWN) continue;
        // 取"出现最多、且不是自己"的那个色。Map 的遍历顺序就是插入顺序（即扫描顺序），
        // 平手时取先扫到的那个 —— 结果与输入完全确定，没有随机量。
        let best = own, bestCount = 0;
        for (const [k, c] of counts) {
          if (k !== own && c > bestCount) { bestCount = c; best = k; }
        }
        if (bestCount >= AVA_CLEAN_MIN) next[i] = best;
      }
    }
    cur = next;
  }
  return cur;
}

// 孤立点计数：与 cleanIsolated 同一条判据 —— 跟上下左右四邻都不同色的格子（只数内点，
// 共 (outSize-2)^2 = 1444 个）。页面用它给调参反馈（"孤立点 M"），主机测试也用它；
// clean 把多少颗脏点换掉了，调档时一眼能看出来。
function countIsolatedCells(idx, outSize) {
  let n = 0;
  for (let y = 1; y < outSize - 1; y++) {
    for (let x = 1; x < outSize - 1; x++) {
      const i = y * outSize + x, v = idx[i];
      if (v !== idx[i - 1] && v !== idx[i + 1] &&
          v !== idx[i - outSize] && v !== idx[i + outSize]) n++;
    }
  }
  return n;
}

/* ---------- 上色 ---------- */

// 工作图 + 调色板 -> outSize x outSize 的索引。两个旋钮管这一阶段：sample / assign。
//
// **assign:"vote"（默认）** —— 格内像素各自先归到调色板里最近的一色，票数过半就填那个色，
// 没有哪个色过半就填"离格内 Lab 均值最近的那个色"。
//
// 为什么不是一律用均值：均值把整格的平方误差最小化，数学上确实最"准"，但它会
// **把小块暗部平均掉** —— 眼珠、鼻孔、叶子的阴影只占格子的一小半，均值被周围的亮色
// 拉走，最后落在一个中间调上，看上去就是"糊"。过半票问的是另一个问题："这一格里
// 最多的像素属于哪个色"，于是 36 颗里 20 颗是暗的，这一格就是暗的。
//
// 为什么要留"没有过半就回均值"这条退路：一格本来就是混的（皮肤渐变、发丝边缘）时，
// 两个相近的色会各自拿到三四成票，纯按票数取会把整片渐变翻成相邻格子来回跳的色块；
// 这时均值才是这格的代表色。6x6=36 是偶数，黑白棋盘这种两色各半的格子也正好走这条路，
// 结果稳定且与遍历顺序无关。assign:"mean" 则是**关掉投票**、一律用均值那条规则。
//
// **sample 决定"格内像素怎么加权"**：area 等权；center 用二维高斯给靠近格心的像素更大
// 权重；point 只取格心那一颗（其余权重 0，等价于最近邻降采样）。投票与均值都按这套权重
// 一起走 —— 于是 point 档下投票和均值必然给出同一个色（只剩一颗像素），这也自洽。
//
// 在 Lab 里算而不是 RGB：RGB 的欧氏距离会把深蓝和深灰算成"很近"，肉眼却一眼分得开。
function drawAvatarIndices(px, work, outSize, palette, features, sample, assign) {
  if (sample === undefined) sample = AVA_SAMPLE_DEFAULT;
  if (assign === undefined) assign = AVA_ASSIGN_DEFAULT;
  const block = work / outSize;
  if (!Number.isInteger(block) || block < 1) throw new Error("工作图必须是输出尺寸的整数倍");

  const count = palette.length;
  const pl = new Float32Array(count), pa = new Float32Array(count), pb = new Float32Array(count);
  for (let c = 0; c < count; c++) {
    rgb2labTo(palette[c][0], palette[c][1], palette[c][2], pl, pa, pb, c);
  }
  // "这一格大半是透明"时填的白（没有白色就用离白最近的那个色）。
  let whitest = 0, whitestD = Infinity;
  for (let c = 0; c < count; c++) {
    const d = labDist(100, 0, 0, pl[c], pa[c], pb[c]);
    if (d < whitestD) { whitestD = d; whitest = c; }
  }
  // 5 位格子 -> 最近的那个调色板色。与取色阶段同一套做法：只给用到的格子算
  // （一张照片一两千个），逐像素投票时查表。
  const nearest = new Uint8Array(AVA_CUBE);
  for (let i = 0; i < features.entries.length; i++) {
    const l = features.L[i], a = features.A[i], b = features.B[i];
    let best = 0, bestD = Infinity;
    for (let c = 0; c < count; c++) {
      const d = labDist(l, a, b, pl[c], pa[c], pb[c]);
      if (d < bestD) { bestD = d; best = c; }
    }
    nearest[features.entries[i].key] = best;
  }

  // 采样核（"这一格怎么变成一个代表色"）：
  //   area   → 36 颗等权（weights = null，走下面那条不乘权重的快路径）；
  //   center → 一维高斯逐轴相乘。sigma = block/6 是量出来的：block/3 太软，只改动
  //            2.5% 的格子、ΔE 几乎不动（旋钮形同虚设），block/2 更甚；block/8~10 又
  //            太硬，逼近点采样。block/6 让"改动的格子比例"正好落在 area 与 point 中间。
  //   point  → 只取格心那一颗（索引 block>>1）。
  const weights = sample === "center" ? (() => {
    const c0 = (block - 1) / 2, sigma = block / 6;
    const w = new Float32Array(block);
    for (let i = 0; i < block; i++) {
      const d = i - c0;
      w[i] = Math.exp(-(d * d) / (2 * sigma * sigma));
    }
    return w;
  })() : null;
  const point = sample === "point";
  const mid = block >> 1;          // block 为偶数时取右下的那一颗当"格心"

  const votes = new Float64Array(count);   // 加权票数：area 时就是像素个数（整数）
  const idx = new Uint8Array(outSize * outSize);
  // "离格内加权 Lab 均值最近的调色板色"。两条 assign 规则都会用到它（vote 是没过半时的
  // 退路，mean 是唯一规则），抽成闭包省得写两遍。
  const nearestToMean = (ml, ma, mb) => {
    let best = 0, bestD = Infinity;
    for (let c = 0; c < count; c++) {
      const d = labDist(ml, ma, mb, pl[c], pa[c], pb[c]);
      if (d < bestD) { bestD = d; best = c; }
    }
    return best;
  };

  for (let by = 0; by < outSize; by++) {
    for (let bx = 0; bx < outSize; bx++) {
      const out = by * outSize + bx;
      const y0 = by * block, x0 = bx * block;
      votes.fill(0);
      let used = 0, wsum = 0, sumL = 0, sumA = 0, sumB = 0;

      if (point) {
        // 只读格心那一颗：就是"最近邻降采样"。它保留细节最多，也最容易碎成雪花。
        // 透明度只由这一颗决定（其余像素根本没看），与下面"半格透明给白"同一意图。
        const o = ((y0 + mid) * work + x0 + mid) * 4;
        if (px[o + 3] < 128) { idx[out] = whitest; continue; }
        const key = cubeKey(px[o], px[o + 1], px[o + 2]);
        const s = features.index[key];
        used = 1; wsum = 1;
        sumL = features.L[s]; sumA = features.A[s]; sumB = features.B[s];
        votes[nearest[key]] = 1;
      } else {
        for (let y = 0; y < block; y++) {
          const row = (y0 + y) * work;
          const wy = weights ? weights[y] : 1;
          for (let x = 0; x < block; x++) {
            const o = (row + x0 + x) * 4;
            if (px[o + 3] < 128) continue;
            const wgt = weights ? wy * weights[x] : 1;
            const key = cubeKey(px[o], px[o + 1], px[o + 2]);
            const s = features.index[key];
            used++;
            wsum += wgt;
            sumL += features.L[s] * wgt; sumA += features.A[s] * wgt; sumB += features.B[s] * wgt;
            votes[nearest[key]] += wgt;
          }
        }
        // 半格以上是透明（抠图边缘）就给白 —— 与"近乎透明的像素不参与取色"同一条规则。
        // 透明度按**像素个数**判（used），不按加权和：抠图软边不该被采样核放大成"这格不透明"。
        if (used * 2 < block * block) { idx[out] = whitest; continue; }
      }

      if (assign === "mean") {
        idx[out] = nearestToMean(sumL / wsum, sumA / wsum, sumB / wsum);
        continue;
      }
      let top = -1, topC = 0;
      for (let c = 0; c < count; c++) if (votes[c] > top) { top = votes[c]; topC = c; }
      // 过半（按加权票）都是这一色：这一格就是它；否则回到均值那条规则。
      idx[out] = (top * 2 > wsum)
        ? topC
        : nearestToMean(sumL / wsum, sumA / wsum, sumB / wsum);
    }
  }
  return idx;
}

// 一张 work x work 的 RGBA 图 -> 16 色调色板 + outSize x outSize 的索引。
//
// params 可以是档位名（"tones16"/"tones12"/"tones8"）或对象：
//   { colors, palette, sample, assign, clean, devicePalette }
// 五个旋钮的语义见文件顶部的常量注释。缺省值合起来就是"用户偏好那一版"：
// 16 色 / 每图自取 / 面积平均 / 过半投票 / **不去孤立点**。
function pixelateAvatar(px, work, outSize, params) {
  const opt = resolveAvatarParams(params);
  const features = avatarFeatures(px, work);        // 直方图与 Lab 一次算好，两个阶段共用
  const palette = buildAvatarPalette(px, work, opt.colors, features, opt);
  const drawn = drawAvatarIndices(px, work, outSize, palette, features, opt.sample, opt.assign);
  // 清洗（去孤立点）只看索引图，不参与取色与上色的任何判断。默认 0 轮 = 原样返回引用。
  const idx = opt.clean > 0 ? cleanIsolated(drawn, outSize, opt.clean) : drawn;
  return { palette: palette, idx: idx };
}

/* ---------- 打包 ---------- */

// 16 色 -> 设备要的 64 字节：每项 4 字节，小端 uint32 的 0x00RRGGBB（第 4 字节是设备
// 打包圆角时自己写的 alpha，这里给 0）。布局与 main/ui_pixel_math.c 的
// ui_pixel_pack_avatar_i4() 一致，也就是 assets/images/love_pixel_art.c 里
// love_pixel_palette[] 的布局。
function packAvatarPalette(palette) {
  const out = new Uint8Array(AVA_COLORS_MAX * 4);
  const view = new DataView(out.buffer);
  for (let i = 0; i < AVA_COLORS_MAX; i++) {
    const c = palette[i] || [0, 0, 0];
    view.setUint32(i * 4, ((c[0] & 255) << 16) | ((c[1] & 255) << 8) | (c[2] & 255), true);
  }
  return out;
}

// packAvatarPalette() 的逆运算：设备回传的 64 字节（base64）-> [[r,g,b] x16]。
//
// 它跟打包放在同一个文件里，是为了让主机测试能钉住"来回一致"这条契约。放在页面里
// 就只能靠肉眼验收 —— 上一版正是这样：长度判据写成了另一个数字（AVA * 4 = 160，
// 而配色只有 64 字节），于是**每一张自带配色都被判成"没有配色"**，缩略图与"拿已有
// 头像当预览源"都静默回落到设备那 16 色图标配色：上传成功，显示的却是另一张图，
// 而且不报错。
//
// 空串、或长度不足，都返回 null（调用方的回退规则：用设备那 16 色图标配色）。
function paletteOf(paletteBase64) {
  if (!paletteBase64) return null;
  const bin = atob(paletteBase64);
  if (bin.length < AVA_COLORS_MAX * 4) return null;
  const out = [];
  for (let i = 0; i < AVA_COLORS_MAX; i++) {
    // 小端 uint32 的 0x00RRGGBB：低字节是 B。
    out.push([bin.charCodeAt(i * 4 + 2), bin.charCodeAt(i * 4 + 1), bin.charCodeAt(i * 4)]);
  }
  return out;
}

// outSize x outSize 的索引 -> 设备要的 4bpp（每字节两个像素，高半字节在前）。
function packAvatar4bpp(idx) {
  const out = new Uint8Array(idx.length / 2);
  for (let i = 0; i < idx.length; i++) {
    if (i % 2 === 0) out[i >> 1] = idx[i] << 4;
    else out[i >> 1] |= idx[i];
  }
  return out;
}
