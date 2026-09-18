// assets/web/avatar_slic.js —— 头像的像素化内核：SLIC 超像素 + 16 色量化。
//
// 单独成文件是为了**能被主机测试直接跑**：tests/test_avatar_slic.mjs 用 node 的 vm
// 加载它、喂合成图、对输出做断言。页面侧由 tools/gen_admin_page.py 把整份文件内联
// 进 admin.js（admin.js 里那一行 SLIC 占位符），所以这里不能有 import/export，
// **也不能定义页面已经有的名字**（AVA / PALETTE / AVATAR_BYTES 都在 admin.js 里），
// 尺寸一律由调用方当参数传进来。
//
// 算法参考 https://github.com/xs7/Pixelate 的 SLIC.js，但重写过：原实现把 Lab 结果写回
// RGBA 数组、用字符串当 clusterID 的字典、每轮 JSON 深拷贝中心，都是浏览器里不该有的开销。

// 工作分辨率相对输出格子的倍数：6 表示每个输出像素覆盖 6x6 个源像素（40 -> 240）。
// 直接在 40x40 上做超像素没有意义 —— 超像素比输出格子还小的时候，投票退化成逐像素取色。
const SLIC_WORK_SCALE = 6;

// 三档"像素化强度"预设，改的是超像素的粒度（在工作图里，6px 的输出格子）与迭代次数。
// weight 是"颜色差多少才算另一个区域"，参考实现里取 1.5 x step：
// 越大越只按颜色分、区域越贴边，越小越像均匀铺开的方格。数值按 outSize=40 调过。
//
// 这里的三个数只是**预置**：页面的高级菜单可以把它们拆开单调（见 params 参数）。
const SLIC_GEARS = {
  soft:   { step: 8,  iters: 5, weight: 12 },
  normal: { step: 14, iters: 6, weight: 21 },
  strong: { step: 22, iters: 8, weight: 33 },
};
// 默认给 soft：步长 8 时一片区域约等于一个输出格子，40x40 的每一格基本都有自己的颜色；
// normal/strong 会成片共用颜色（14 → 约 17x17 片，22 → 约 11x11 片），看着更"平"。
const DEFAULT_GEAR = "soft";

// 每个输出格子最终填什么颜色。
//
// 先分清两件事：**空间分辨率由 step 决定**（步长 S 比输出格子 6px 大多少，就有多少个
// 输出格子共用一片区域、共用同一个颜色）；mode 只决定"那片区域该是什么色"。
// 想让画面有细节，主要是把 step 调小，而不是换 mode —— 这一点我一开始也搞反过。
//
//   cell   这一格里、属于主导区域的那些像素的**平均色**。同一片区域里相邻格子会各自
//          跟着局部明暗走 → 细节最多；又因为在格子内平均，不会像逐像素取色那样起噪点。
//   center 主导区域的**中心那一个原像素**的颜色（参考实现的做法）。对比比均值强，
//          但**同区域的格子仍然是同一个颜色**，不增加空间分辨率。
//   vote   主导区域里出现最多的**调色板色**（最典型，边界上容易挑到亮色）。
//   mean   主导区域的**平均色**（同区域必然同色，最平；最初的默认，被用户评价为
//          "完全没有细节"）。
const DEFAULT_MODE = "cell";
const SLIC_MODES = ["cell", "center", "vote", "mean"];

// sRGB -> 线性的查找表。Lab 转换里那两次幂运算占了整张图的大头，查表省掉其中最贵的一次。
const SRGB_LINEAR = (() => {
  const t = new Float32Array(256);
  for (let i = 0; i < 256; i++) {
    const c = i / 255;
    t[i] = c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
  }
  return t;
})();

// sRGB -> CIELAB(D65)。写进三个并排的数组而不是返回对象：57600 个像素每像素造一个
// 对象既慢又费内存。
function rgb2labTo(r, g, b, L, A, B, at) {
  const R = SRGB_LINEAR[r], G = SRGB_LINEAR[g], Bl = SRGB_LINEAR[b];
  const x = (R * 0.4124564 + G * 0.3575761 + Bl * 0.1804375) / 0.950456;
  const y = R * 0.2126729 + G * 0.7151522 + Bl * 0.0721750;
  const z = (R * 0.0193339 + G * 0.1191920 + Bl * 0.9503041) / 1.088754;
  const fx = x > 0.008856 ? Math.cbrt(x) : (7.787 * x + 16 / 116);
  const fy = y > 0.008856 ? Math.cbrt(y) : (7.787 * y + 16 / 116);
  const fz = z > 0.008856 ? Math.cbrt(z) : (7.787 * z + 16 / 116);
  L[at] = 116 * fy - 16;
  A[at] = 500 * (fx - fy);
  B[at] = 200 * (fy - fz);
}

// 调色板 -> Lab（一次调用算一遍，16 个颜色可以忽略不计）。
function paletteToLab(palette) {
  const L = new Float32Array(palette.length);
  const A = new Float32Array(palette.length);
  const B = new Float32Array(palette.length);
  for (let i = 0; i < palette.length; i++) {
    rgb2labTo(palette[i][0], palette[i][1], palette[i][2], L, A, B, i);
  }
  return { L, A, B };
}

// 比色在 Lab 里做：RGB 的欧氏距离会把深蓝和深灰算成"很近"，而人眼一眼就能分开。
function nearestPaletteLab(l, a, b, palLab) {
  let best = 0, bestD = Infinity;
  for (let i = 0; i < palLab.L.length; i++) {
    const dl = l - palLab.L[i], da = a - palLab.A[i], db = b - palLab.B[i];
    const d = dl * dl + da * da + db * db;
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
}

// 把档位名或半成品参数补全成一组可用参数（页面上的高级菜单直接调这三个数 + mode）。
function resolveParams(params) {
  const base = typeof params === "string"
    ? (SLIC_GEARS[params] || SLIC_GEARS[DEFAULT_GEAR])
    : (params || SLIC_GEARS[DEFAULT_GEAR]);
  const gear = SLIC_GEARS[DEFAULT_GEAR];
  const clamp = (v, lo, hi, dflt) => {
    const n = Number(v);
    return Number.isFinite(n) ? Math.min(hi, Math.max(lo, n)) : dflt;
  };
  return {
    step: clamp(base.step, 2, 64, gear.step),
    iters: Math.round(clamp(base.iters, 1, 20, gear.iters)),
    weight: clamp(base.weight, 1, 128, gear.weight),
    mode: SLIC_MODES.includes(base.mode) ? base.mode : DEFAULT_MODE,
  };
}

// 一张 work x work 的 RGBA 图 -> outSize x outSize 的调色板索引。
//
// 分三步：
//   1. sRGB -> Lab（alpha < 128 的像素不参与聚类）
//   2. SLIC：网格播种的中心在"Lab 距离 + 位置距离"下反复迭代，收敛成一片片颜色相近的区域
//   3. 每个输出格子做多数投票选出主导区域，再把该区域"某个代表色"映射到调色板
// 为什么不是逐像素取最近色：那是每个像素各自挑一个颜色，照片的渐变会碎成噪点；
// 先按区域合并再上色，色块才会沿着脸、头发、背景的边界走 —— 这就是"像素画"和"马赛克"的区别。
//
// params 可以是档位名（"soft"/"normal"/"strong"）或对象 {step, iters, weight, mode}。
// `mode` 决定第 3 步用区域的哪个代表色，是"还剩下多少细节"最要紧的旋钮（见 SLIC_MODES）。
function slicPixelate(px, outSize, palette, params) {
  const gear = resolveParams(params);
  const palLab = paletteToLab(palette);
  const work = outSize * SLIC_WORK_SCALE;   // 工作分辨率(像素)
  const block = work / outSize;             // 每个输出格子覆盖多少源像素
  const n = work * work;
  const L = new Float32Array(n), A = new Float32Array(n), B = new Float32Array(n);
  const opaque = new Uint8Array(n);
  for (let i = 0; i < n; i++) {
    const o = i * 4;
    if (px[o + 3] < 128) continue;
    opaque[i] = 1;
    rgb2labTo(px[o], px[o + 1], px[o + 2], L, A, B, i);
  }

  // vote 模式要按"调色板色"投票，所以先把每个像素量化到调色板索引（每像素一次）。
  // 只在需要时才付这份开销：center/mean 直接比 Lab 距离，用不上。
  let pixelIdx = null;
  if (gear.mode === "vote") {
    pixelIdx = new Uint8Array(n);
    for (let i = 0; i < n; i++) {
      if (!opaque[i]) continue;
      pixelIdx[i] = nearestPaletteLab(L[i], A[i], B[i], palLab);
    }
  }

  const stepN = Math.max(1, Math.round(work / gear.step));
  const S = work / stepN;                 // 实际网格间距（取整后与请求值略有出入）
  const count = stepN * stepN;
  const cl = new Float32Array(count), ca = new Float32Array(count), cb = new Float32Array(count);
  const cx = new Float32Array(count), cy = new Float32Array(count);

  // 播种：先落在网格点上，再挪到 3x3 邻域里亮度最平的那个像素 —— 中心要是压在
  // 一条边上，这个区域会被拉成跨两边的一半一半。
  let k = 0;
  for (let gy = 0; gy < stepN; gy++) {
    for (let gx = 0; gx < stepN; gx++, k++) {
      const x0 = Math.min(work - 1, Math.floor((gx + 0.5) * S));
      const y0 = Math.min(work - 1, Math.floor((gy + 0.5) * S));
      let bx = x0, by = y0, bestGrad = Infinity;
      const ymin = Math.max(0, y0 - 1), ymax = Math.min(work - 1, y0 + 1);
      const xmin = Math.max(0, x0 - 1), xmax = Math.min(work - 1, x0 + 1);
      for (let y = ymin; y <= ymax; y++) {
        for (let x = xmin; x <= xmax; x++) {
          const i = y * work + x;
          if (!opaque[i]) continue;
          const g = Math.abs(L[i] - L[y * work + Math.min(work - 1, x + 1)]) +
                    Math.abs(L[i] - L[Math.min(work - 1, y + 1) * work + x]);
          if (g < bestGrad) { bestGrad = g; bx = x; by = y; }
        }
      }
      cx[k] = bx; cy[k] = by;
      cl[k] = L[by * work + bx]; ca[k] = A[by * work + bx]; cb[k] = B[by * work + bx];
    }
  }

  const win = Math.ceil(S);               // 搜索窗口半径：每个像素只看附近 ±S 的中心
  const dist = new Float32Array(n);
  const owner = new Int32Array(n);
  const sumL = new Float64Array(count), sumA = new Float64Array(count), sumB = new Float64Array(count);
  const sumX = new Float64Array(count), sumY = new Float64Array(count), sumN = new Uint32Array(count);
  const w2 = gear.weight * gear.weight, s2 = S * S;

  for (let it = 0; it < gear.iters; it++) {
    dist.fill(Infinity);
    owner.fill(-1);
    for (let c = 0; c < count; c++) {
      const cxc = cx[c], cyc = cy[c];
      const x0 = Math.max(0, Math.floor(cxc) - win), x1 = Math.min(work - 1, Math.floor(cxc) + win);
      const y0 = Math.max(0, Math.floor(cyc) - win), y1 = Math.min(work - 1, Math.floor(cyc) + win);
      const clc = cl[c], cac = ca[c], cbc = cb[c];
      for (let y = y0; y <= y1; y++) {
        const row = y * work;
        const dy = y - cyc;
        for (let x = x0; x <= x1; x++) {
          const i = row + x;
          if (!opaque[i]) continue;
          const dl = L[i] - clc, da = A[i] - cac, db = B[i] - cbc;
          const dx = x - cxc;
          const d = (dl * dl + da * da + db * db) / w2 + (dx * dx + dy * dy) / s2;
          if (d < dist[i]) { dist[i] = d; owner[i] = c; }
        }
      }
    }

    sumL.fill(0); sumA.fill(0); sumB.fill(0); sumX.fill(0); sumY.fill(0); sumN.fill(0);
    for (let i = 0; i < n; i++) {
      const c = owner[i];
      if (c < 0) continue;
      sumL[c] += L[i]; sumA[c] += A[i]; sumB[c] += B[i];
      sumX[c] += i % work; sumY[c] += (i / work) | 0; sumN[c]++;
    }
    for (let c = 0; c < count; c++) {
      if (!sumN[c]) continue;              // 空区域保留上一轮的中心，别让它漂走
      cl[c] = sumL[c] / sumN[c]; ca[c] = sumA[c] / sumN[c]; cb[c] = sumB[c] / sumN[c];
      cx[c] = sumX[c] / sumN[c]; cy[c] = sumY[c] / sumN[c];
    }
  }

  // 每个输出格子：先数出落在哪个区域里的像素最多（主导区域），再按 mode 取那个区域的代表色。
  //
  // 三种模式差的不是聚类结果，而是"这一格最后填哪一个颜色"：
  //   center 直接采主导区域中心那个**原像素**的颜色 —— 同一片区域里相邻格子会各自采到
  //          不同的像素，于是照片的明暗起伏还在（参考实现就是这么做的）；
  //   vote   在主导区域的像素里数"哪个调色板色出现得最多"，是这片区域最典型的色；
  //   mean   区域平均色 —— 同一片区域的所有格子会拿到同一个颜色，大块平涂，细节最少。
  const idx = new Uint8Array(outSize * outSize);
  const votes = new Uint32Array(count);
  const colorVotes = new Uint32Array(palette.length);
  for (let by = 0; by < outSize; by++) {
    for (let bx = 0; bx < outSize; bx++) {
      votes.fill(0);
      let best = -1, bestVotes = 0, seen = 0;
      for (let y = by * block; y < (by + 1) * block; y++) {
        const row = y * work;
        for (let x = bx * block; x < (bx + 1) * block; x++) {
          const i = row + x;
          if (!opaque[i]) continue;
          seen++;
          const c = owner[i];
          if (c < 0) continue;
          const v = ++votes[c];
          if (v > bestVotes) { bestVotes = v; best = c; }
        }
      }
      // 半格以上是透明（抠图边缘）就给白色 —— 与"近乎透明的像素给白色"同一条规则。
      // 索引 1 = W：调色板顺序被 tests/test_love_pixel_art_gen.py 钉死，不能改。
      if (best < 0 || seen * 2 < block * block) { idx[by * outSize + bx] = 1; continue; }

      if (gear.mode === "mean") {
        idx[by * outSize + bx] = nearestPaletteLab(cl[best], ca[best], cb[best], palLab);
        continue;
      }

      // cell / vote 都要再看一遍这一格里"属于主导区域"的像素：一个累计 Lab 求均值，
      // 一个按调色板索引投票。两遍合并成一遍算。
      let sumL = 0, sumA = 0, sumB = 0, sumN = 0;
      if (gear.mode === "cell") {
        for (let y = by * block; y < (by + 1) * block; y++) {
          const row = y * work;
          for (let x = bx * block; x < (bx + 1) * block; x++) {
            const i = row + x;
            if (!opaque[i] || owner[i] !== best) continue;
            sumL += L[i]; sumA += A[i]; sumB += B[i]; sumN++;
          }
        }
        idx[by * outSize + bx] = sumN
          ? nearestPaletteLab(sumL / sumN, sumA / sumN, sumB / sumN, palLab)
          : nearestPaletteLab(cl[best], ca[best], cb[best], palLab);
        continue;
      }

      if (gear.mode === "vote") {
        colorVotes.fill(0);
        let bestColor = 1, bestColorVotes = 0;
        for (let y = by * block; y < (by + 1) * block; y++) {
          const row = y * work;
          for (let x = bx * block; x < (bx + 1) * block; x++) {
            const i = row + x;
            if (!opaque[i] || owner[i] !== best) continue;
            const v = ++colorVotes[pixelIdx[i]];
            if (v > bestColorVotes) { bestColorVotes = v; bestColor = pixelIdx[i]; }
          }
        }
        idx[by * outSize + bx] = bestColor;
        continue;
      }

      // center：主导区域中心的那个原像素。中心是浮点（一轮轮算出来的均值），取整即可；
      // 越界时夹回图内 —— 边界上的区域中心可能落在外面半像素。
      const mx = Math.min(work - 1, Math.max(0, Math.round(cx[best])));
      const my = Math.min(work - 1, Math.max(0, Math.round(cy[best])));
      const mi = my * work + mx;
      const mo = mi * 4;
      idx[by * outSize + bx] = opaque[mi]
        ? nearestPaletteLab(L[mi], A[mi], B[mi], palLab)
        : nearestPaletteLab(px[mo] , 0, 0, palLab);   // 中心落在透明像素上时的兜底
    }
  }
  return idx;
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
