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

// 三档"像素化强度"，改的是超像素的粒度（在工作图里，6px 的输出格子）与迭代次数。
// weight 是"颜色差多少才算另一个区域"，参考实现里取 1.5 x step：
// 越大越只按颜色分、区域越贴边，越小越像均匀铺开的方格。数值按 outSize=40 调过。
const SLIC_GEARS = {
  soft:   { step: 8,  iters: 5, weight: 12 },
  normal: { step: 14, iters: 6, weight: 21 },
  strong: { step: 22, iters: 8, weight: 33 },
};
const DEFAULT_GEAR = "normal";

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

// 一张 work x work 的 RGBA 图 -> outSize x outSize 的调色板索引。
//
// 分三步：
//   1. sRGB -> Lab（alpha < 128 的像素不参与聚类）
//   2. SLIC：网格播种的中心在"Lab 距离 + 位置距离"下反复迭代，收敛成一片片颜色相近的区域
//   3. 每个输出格子做多数投票，取那个区域的**平均色**填满整格，再映射到调色板
// 为什么不是逐像素取最近色：那是每个像素各自挑一个颜色，照片的渐变会碎成噪点；
// 先按区域合并再上色，色块才会沿着脸、头发、背景的边界走 —— 这就是"像素画"和"马赛克"的区别。
function slicPixelate(px, outSize, palette, gearName) {
  const gear = SLIC_GEARS[gearName] || SLIC_GEARS[DEFAULT_GEAR];
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

  // 每个输出格子：数一数落在各区域的像素，取最多的那个，用它的平均色。
  const idx = new Uint8Array(outSize * outSize);
  const votes = new Uint32Array(count);
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
      idx[by * outSize + bx] = nearestPaletteLab(cl[best], ca[best], cb[best], palLab);
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
