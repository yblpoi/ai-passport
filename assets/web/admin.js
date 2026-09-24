// assets/web/admin.js —— 后台网页脚本,由设备以 /admin.js 单独返回。
// 图标是内联的 data URI(一共两千多字节)。不要改回 /icon/N.png 那种独立资源:
// 一次页面加载会多出十六个并发请求,把设备那点堆压到 Wi-Fi 驱动发不出帧。
const ICONS = __ICONS_JSON__;
// 设备端的 16 色图标配色,顺序即索引顺序,必须与设备一致。新上传的头像**自带**自己的
// 16 色(见 avatar_pixel.js),这张表只在两种情况下用:老头像(没有自带配色)、以及
// 内置图标。规则与设备端 love_store_load_avatar_palette() 的回退一致。
const PALETTE = __PALETTE_JSON__;
const AVA = 40;                       // 与设备端 LOVE_ICON_PX 一致
const AVATAR_BYTES = AVA * AVA / 2;   // 40x40 4bpp = 800 字节
const AVATAR_MAX = 4;
const TRANSPARENT_PX = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";

const byId = (id) => document.getElementById(id);

// 设备回传的自定义头像(4bpp base64,每个槽位一个,空串表示没上传)
let AVATARS = new Array(AVATAR_MAX).fill("");
// 每个槽位自带的 16 色调色板(64 字节 base64)。空串 = 这张头像用设备那 16 色图标配色
// (老头像、或只传了索引的那种),与设备端 love_store_load_avatar_palette() 的规则一致。
let AVATAR_PALETTES = new Array(AVATAR_MAX).fill("");

let model = { start:"", blankOff:30, people:[{name:"",icon:0},{name:"",icon:1}], events:[] };

function toast(msg){ const t = byId("toast"); t.textContent = msg; t.classList.add("show");
  clearTimeout(t._h); t._h = setTimeout(()=>t.classList.remove("show"), 2200); }

async function api(path, options){
  const res = await fetch(path, Object.assign({headers:{"Content-Type":"application/json"}}, options));
  if(!res.ok) throw new Error(await res.text() || res.statusText);
  return res.headers.get("Content-Type")?.includes("json") ? res.json() : res.text();
}

/* ---------- 自定义头像：网页压缩 -> 设备存 4bpp ---------- */
// 像素化的内核在 assets/web/avatar_pixel.js（每图自带 16 色 + 逐格选色），由
// tools/gen_admin_page.py 内联在下面这一行。它单独成文件是为了能被主机测试直接跑
// （tests/test_avatar_pixel.mjs）—— 这段数学要是只活在浏览器里，就只能靠肉眼验收了。
__AVATAR_PIXEL_JS__

// 选中的图片 -> 居中裁成正方形 -> 240x240 工作图 -> 取色 + 上色 -> 864 字节
// （64 字节调色板 + 800 字节 4bpp），一次 POST 传给设备。
//
// 用 <img> 而不是 createImageBitmap 解码:iPhone 相册默认是 HEIC,Safari 的
// createImageBitmap 对它的支持比 <img> 窄得多;走 <img> 就是走浏览器自己的
// 图像管线,手机直接拍的照片也能选。解码失败时给一句人话,不要把原始异常抛到
// toast 里。
async function compressAvatar(file, params){
  const url = URL.createObjectURL(file);
  try {
    const bitmap = await new Promise((resolve, reject) => {
      const el = new Image();
      el.onload = () => resolve(el);
      el.onerror = () => reject(new Error("浏览器解不开这张图片，试试转成 PNG 或 JPEG"));
      el.src = url;
    });

    const w = bitmap.naturalWidth, h = bitmap.naturalHeight;
    const side = Math.min(w, h);
    const work = AVA * AVA_WORK_SCALE;
    const canvas = document.createElement("canvas");
    canvas.width = work; canvas.height = work;
    const ctx = canvas.getContext("2d", { willReadFrequently: true });
    // 先居中裁成正方形再缩放，避免把脸拉扁（设备端只做满幅方角 + 4px 圆角，圆角在
    // 解码时切；网页预览用 CSS 的 border-radius 对齐）。
    ctx.drawImage(bitmap, (w - side) / 2, (h - side) / 2, side, side, 0, 0, work, work);
    const px = ctx.getImageData(0, 0, work, work).data;

    // 留一份工作图给高级参数的实时预览用：调滑块时不需要重新解码/缩放原图。
    previewPx = px;
    previewSourceNote = "";
    return avatarBytes(px, params);
  } finally {
    URL.revokeObjectURL(url);
  }
}

// 工作图 -> 设备要的那串字节（864 = 64 配色 + 800 索引，与 main/love_httpd.c 对齐）。
function avatarBytes(px, params){
  const done = pixelateAvatar(px, AVA * AVA_WORK_SCALE, AVA, params);
  const palette = packAvatarPalette(done.palette);
  const indices = packAvatar4bpp(done.idx);
  const out = new Uint8Array(palette.length + indices.length);
  out.set(palette, 0);
  out.set(indices, palette.length);
  return out;
}

// 设备回传的 64 字节配色 -> [[r,g,b] x16]，实现在 avatar_pixel.js 的 paletteOf()：
// 它是"打包成 64 字节"的逆运算，两半必须待在一起、由同一条主机测试钉住来回一致。
// 空串（老头像、或网页只传了索引）返回 null，调用方回落到设备那 16 色图标配色 ——
// 这也是设备端的规则，两边必须一致。

/* ---------- 高级参数：像素化怎么算（照参考实现的 dat.GUI，多了实时预览） ---------- */
//
// 五个旋钮的语义与默认值都定义在 avatar_pixel.js 的 resolveAvatarParams() 里，这里只
// 负责把界面对上号。默认那一档就是用户偏好的那版：细节最全、不做去孤立点（去孤立点会
// 吃掉一部分真细节，取舍归用户，见内核顶部说明）。
const ADV_DEFAULTS = { colors: AVA_COLORS_DEFAULT, palette: "photo", sample: "area",
                       assign: "vote", clean: 0 };

// 三档预设 = 一整组参数（不再是"只改取色数量"）。三档都在默认之上沿"细节 vs 干净"这根轴
// 移动：越往下色越少、去孤立点越多。改过任一旋钮都会落到"自定义"，见 advMarkGear()。
const AVATAR_PRESETS = {
  natural:  { colors: 16, palette: "photo", sample: "area", assign: "vote", clean: 0 },
  balanced: { colors: 12, palette: "photo", sample: "area", assign: "vote", clean: 1 },
  pixel:    { colors: 8,  palette: "photo", sample: "area", assign: "vote", clean: 2 },
};
const AVATAR_PRESET_DEFAULT = "natural";

let avatarParams = Object.assign({}, ADV_DEFAULTS);
let previewPx = null;          // 240x240 的工作图（选过照片、或取自现有头像）
let previewSourceNote = "";    // 预览源是"刚选的照片"还是"设备上已有的头像"
let previewTimer = 0;
// 最后一次上传（槽位 + 原始 File）。调完参数要能"再传一次"，否则每改一次参数都得
// 重新走一遍相册选择（手机上尤其烦）。
let lastAvatarUpload = null;

// 交给内核的那份参数：把设备那 16 色一并带上（palette:"device" 时才用得到）。内核里
// 没有 PALETTE 这个名字，只认 params.devicePalette —— 见 avatar_pixel.js。
function avatarKernelParams(){
  return Object.assign({}, avatarParams, { devicePalette: PALETTE });
}

function advSyncLabels(){
  byId("advColorsVal").textContent = avatarParams.colors;
  byId("advColors").value = avatarParams.colors;
  byId("advPalette").value = avatarParams.palette;
  byId("advSample").value = avatarParams.sample;
  byId("advAssign").value = avatarParams.assign;
  byId("advCleanVal").textContent = avatarParams.clean;
  byId("advClean").value = avatarParams.clean;
}

// 档位与旋钮是一回事：选档 = 把预设抄进五个旋钮；动过任一旋钮就显示"自定义"。
// 比的是五个字段——早先只比 colors 一个数，现在多了配色来源/采样/上色/清洗，只比一个会漏。
function advMarkGear(){
  for (const [name, preset] of Object.entries(AVATAR_PRESETS)){
    if (Object.keys(preset).every((k) => preset[k] === avatarParams[k])){
      byId("avatarStrength").value = name; return;
    }
  }
  byId("avatarStrength").value = "custom";
}

function advApplyPreset(name){
  const preset = AVATAR_PRESETS[name];
  if (!preset) return;
  avatarParams = Object.assign({}, preset);
  advSyncLabels();
  advMarkGear();          // 让档位下拉自己回到这个预设（而不是依赖调用方先把 select 设好）
  advSchedulePreview();
}

function advFromInputs(){
  avatarParams.colors = Number(byId("advColors").value);
  avatarParams.palette = byId("advPalette").value;
  avatarParams.sample = byId("advSample").value;
  avatarParams.assign = byId("advAssign").value;
  avatarParams.clean = Number(byId("advClean").value);
  advSyncLabels();
  advMarkGear();
  advSchedulePreview();
}

// 预览：把 40x40 的索引按这张图自己的 16 色画出来，放大 4 倍（像素风要最近邻，
// CSS 的 image-rendering: pixelated 保证放大不发虚）。
//
// 提示行给三个数：耗时 / 用色数 / 孤立点数。后两个正是调参时唯一能看见反馈的量 ——
// "用色"是这张 40x40 实际用到几种色，"孤立点"是去孤立点那个旋钮在处理的量。少了它们，
// 拖滑块只能靠肉眼在 160px 的画布上猜。
function advRenderPreview(){
  const canvas = byId("advPreview");
  if (!previewPx){
    canvas.style.display = "none";
    byId("advHint").textContent = "先选一张照片，或直接调参数看看效果。";
    return;
  }
  const t0 = performance.now();
  const done = pixelateAvatar(previewPx, AVA * AVA_WORK_SCALE, AVA, avatarKernelParams());
  const ms = Math.round(performance.now() - t0);
  const scale = 4, side = AVA * scale;
  canvas.width = side; canvas.height = side;
  canvas.style.display = "";
  const ctx = canvas.getContext("2d");
  const img = ctx.createImageData(side, side);
  for (let y = 0; y < side; y++){
    for (let x = 0; x < side; x++){
      const p = done.palette[done.idx[((y / scale) | 0) * AVA + ((x / scale) | 0)]] || [0, 0, 0];
      const o = (y * side + x) * 4;
      img.data[o] = p[0]; img.data[o + 1] = p[1]; img.data[o + 2] = p[2]; img.data[o + 3] = 255;
    }
  }
  ctx.putImageData(img, 0, 0);
  const usedColors = new Set(done.idx).size;
  const isolated = countIsolatedCells(done.idx, AVA);
  byId("advHint").textContent = "预览：算完 " + ms + " ms / 用色 " + usedColors +
                                " / 孤立点 " + isolated + "。" + previewSourceNote;
}

function advSchedulePreview(){
  clearTimeout(previewTimer);
  previewTimer = setTimeout(advRenderPreview, 120);   // 拖滑块时别每像素都重算
}

// 槽位 -> 画它该用的 16 色：设备回传的自带配色，没有就是设备那 16 色图标配色。
function slotPalette(slot){
  return paletteOf(AVATAR_PALETTES[slot]) || PALETTE;
}

// 没选过照片时，用设备上已有的某个自定义头像当预览源：把 4bpp 解成 40x40 索引、
// 再按 6 倍放大成 240x240（相当于"如果拿这张头像再走一遍像素化"）。
function advPreviewFromAvatar(){
  if (previewPx) return;
  const slot = AVATARS.findIndex((a) => a);
  if (slot < 0) return;
  const bin = atob(AVATARS[slot]);
  if (bin.length < AVATAR_BYTES) return;
  const colors = slotPalette(slot);
  const work = AVA * AVA_WORK_SCALE, scale = AVA_WORK_SCALE;
  previewPx = new Uint8ClampedArray(work * work * 4);
  for (let y = 0; y < work; y++){
    for (let x = 0; x < work; x++){
      const cell = ((y / scale) | 0) * AVA + ((x / scale) | 0);
      const byte = bin.charCodeAt(cell >> 1);
      const index = (cell % 2 === 0) ? (byte >> 4) : (byte & 0x0F);
      const p = colors[index] || [0, 0, 0];
      const o = (y * work + x) * 4;
      previewPx[o] = p[0]; previewPx[o + 1] = p[1]; previewPx[o + 2] = p[2]; previewPx[o + 3] = 255;
    }
  }
  previewSourceNote = "（当前用的是设备上已有的头像）";
}

// 设备回传的 4bpp base64 -> 可直接显示的 data URI(选择器里的缩略图)
function avatarThumb(base64, paletteBase64){
  if(!base64) return TRANSPARENT_PX;
  const bin = atob(base64);
  if(bin.length < AVATAR_BYTES) return TRANSPARENT_PX;
  const colors = paletteOf(paletteBase64) || PALETTE;

  const canvas = document.createElement("canvas");
  canvas.width = AVA; canvas.height = AVA;
  const ctx = canvas.getContext("2d");
  const img = ctx.createImageData(AVA, AVA);
  for(let i = 0; i < AVA * AVA; i++){
    const byte = bin.charCodeAt(i >> 1);
    const idx = (i % 2 === 0) ? (byte >> 4) : (byte & 0x0F);
    const p = colors[idx] || [0, 0, 0];
    img.data[i*4] = p[0]; img.data[i*4 + 1] = p[1]; img.data[i*4 + 2] = p[2];
    img.data[i*4 + 3] = 255;
  }
  ctx.putImageData(img, 0, 0);
  return canvas.toDataURL("image/png");
}

// 图标号 -> 图片地址。0..15 是内置素材(/icon/NN.png),16..19 是自定义头像槽位。
// 槽位还没上传数据时回落到第 0 个内置图标 —— 与设备端 resolve_icon() 的行为一致,
// 否则预览会显示成一个空白头像,和真机对不上。
function iconSrc(idx){
  if(idx < ICONS.length) return ICONS[idx].data;
  const slot = idx - ICONS.length;
  const data = AVATARS[slot];
  return data ? avatarThumb(data, AVATAR_PALETTES[slot]) : ICONS[0].data;
}

function iconLabel(idx){
  return idx < ICONS.length ? ICONS[idx].label : `自定义 ${idx - ICONS.length + 1}`;
}

// 只有自定义头像(照片,满幅方角)加圆角。内置图标是透明背景的图形,给它加圆角
// 会切到描边(实测 8 个图标的四角各有描边,合计 48 个像素),而设备端刻意不做这件事
// —— 两端必须一致,否则网页预览会显示成设备上没有的样子。
function setAvatarImg(el, idx){
  el.src = iconSrc(idx);
  el.classList.toggle("rounded", idx >= ICONS.length);
}

async function uploadAvatar(slot, file){
  const bytes = await compressAvatar(file, avatarKernelParams());
  lastAvatarUpload = { slot: slot, file: file };   // 让"用当前参数重新上传"可用
  const again = byId("advReupload");
  if (again) again.hidden = false;
  const res = await fetch(`/api/avatar?slot=${slot}`, {
    method: "POST",
    headers: { "Content-Type": "application/octet-stream" },
    body: bytes,
  });
  if(!res.ok){
    const text = await res.text().catch(()=> "");
    throw new Error(text || res.statusText);
  }
  return res.json();
}

async function clearAvatar(slot){
  const res = await fetch(`/api/avatar/clear?slot=${slot}`, { method: "POST", body: "{}" });
  if(!res.ok) throw new Error(res.statusText);
  return res.json();
}

// 常驻一个已挂进文档的隐藏文件选择器。
// 不要像早先那样每次现造一个游离的 input 再 click():iOS Safari 会忽略对
// 未挂载元素的 click(),表现就是"第一次点了没反应,要再点一次"。
const avatarInput = document.createElement("input");
avatarInput.type = "file";
avatarInput.accept = "image/*";
avatarInput.hidden = true;
document.body.appendChild(avatarInput);

// 触发文件选择 -> 压缩 -> 上传。成功后把该槽位设为当前选中图标。
function pickAvatarFile(onDone){
  // 先清空,否则连续两次选同一个文件不会触发 change
  avatarInput.value = "";
  avatarInput.onchange = async () => {
    const file = avatarInput.files && avatarInput.files[0];
    if(!file) return;
    try{
      await onDone(file);
      // 这张照片现在就是高级参数的预览源了，顺手把预览刷出来（compressAvatar 已经把
      // 240x240 的工作图留在 previewPx 里）。
      if (byId("advBox") && byId("advBox").open) advSchedulePreview();
    }catch(e){
      toast("上传失败：" + e.message);
    }
  };
  avatarInput.click();
}

// 倒计时与农历的判据和设备端是同一件事,内核单独成文(assets/web/preview_math.js),
// 由生成器内联到这一行 —— 页面仍然只有 /admin.js 一个请求,而测试与维护都只有一份实现。
__PREVIEW_MATH_JS__

function currentToday(){
  return lastTime && lastTime.synced ? todayFrom(lastTime.epoch) : todayFrom(Math.floor(Date.now()/1000));
}

function renderPreview(){
  const today = currentToday();
  const start = parseDate(model.start);
  // 与设备端 love_days_together() 同一条规则:含起始日当天;今天早于起始日时是 0,
  // 不显示负数。这条曾经和真机不一致,现在由 tests/vectors/date_vectors.json 钉住。
  byId("pvDays").textContent = start ? Math.max(0, daysBetween(start, today) + 1) : "--";
  byId("pvStart").textContent = "起始日 " + (model.start || "----");
  // 设备主屏不再显示对时时间,只用单位那行标注时间状态。
  // 提示:设备侧还可能拿断电前的天数快照顶上,那种情况显示"天(未对时)",这里判断不到。
  byId("pvUnit").textContent = lastTime && lastTime.synced ? "天" : "天(未对时)";
  const soc = typeof lastBattery === "number" && lastBattery >= 0 ? lastBattery : null;
  byId("pvBattery").textContent = soc === null ? "-- %" : soc + "%";
  byId("pvBatFill").style.width = (soc === null ? 0 : soc) + "%";
  setAvatarImg(byId("pvIconA"), model.people[0].icon);
  setAvatarImg(byId("pvIconB"), model.people[1].icon);
  byId("pvNameA").textContent = model.people[0].name || "TA";
  byId("pvNameB").textContent = model.people[1].name || "TA";

  // 事件卡预览:优先取**第一条单页事件** —— 真机上卡片只可能是"单页"事件,
  // 列表事件不会单独成卡。一条单页事件都没有时退回第一条,当样例看看排版。
  const e = model.events.find((ev) => (ev.viewMode ?? 0) === 1) || model.events[0];
  if(e){
    const synced = !!(lastTime && lastTime.synced);
    let target = null;
    let dateLine = "目标日 ----";

    if(e.kind === 2){
      // 农历:按农历月日找下一次;折算不出公历就只报农历,不给假日期
      const lname = lunarName(e.lunarMonth, e.lunarDay);
      target = nextLunar(today, e.lunarMonth, e.lunarDay);
      dateLine = target
        ? `农历${lname} ${target.y}-${String(target.m).padStart(2,"0")}-${String(target.d).padStart(2,"0")}`
        : `农历${lname}`;
    } else {
      const d = parseDate(e.date);
      target = e.kind === 0 ? nextOccurrence(today, d.m, d.d) : d;
      dateLine = "目标日 " + target.y + "-" +
        String(target.m).padStart(2,"0") + "-" + String(target.d).padStart(2,"0");
    }

    const diff = target ? daysBetween(today, target) : 0;
    byId("pvEventName").textContent = e.name || "纪念日";
    byId("pvEventDays").textContent = (synced && target) ? Math.abs(diff) : "--";
    byId("pvEventUnit").textContent = !synced ? "未同步"
                                      : (!target ? "农历超出范围" : (diff >= 0 ? "天后" : "天前"));
    byId("pvEventDate").textContent = dateLine;
    setAvatarImg(byId("pvEventIcon"), e.icon);
  }
  byId("eventCount").textContent = model.events.length ? `共 ${model.events.length} 条` : "还没有事件";
  updatePreviewPage();
  updatePreviewHint();
}

// 底部提示行要跟真机一致:设备上上/下 一律是"翻页"(单页卡与列表页都在同一个轮播上)。
// 单独成函数是因为它同时依赖"当前预览哪个屏"和"有没有可翻的页",两边都要能触发刷新。
function updatePreviewHint(){
  const main = byId("pvMainView").style.display !== "none";
  // 真机主屏只在环上真有页时才提示上/下;一页都没有时上/下 什么都不做。
  byId("pvHint").textContent = (main && ringPageTotal() === 0)
    ? "长按确定 设置"
    : "上/下 翻页 · 长按确定 设置";
}

// 真机的页码是**整套轮播**的编号(规则与 main/love_view.h 一致):单页事件各占一页,
// 列表事件每 4 条一页,合起来就是分母。
function ringPageTotal(){
  const cards = model.events.filter((e) => (e.viewMode ?? 0) === 1).length;
  const list = model.events.filter((e) => (e.viewMode ?? 0) === 0).length;
  return cards + Math.ceil(list / 4);
}

// 预览画的卡片是第一张单页卡(真机上只有"单页"事件才有卡片),所以序号恒为 1。
// 没有单页事件时真机上根本没有卡片页,连页码一起隐藏 —— 不留一个假编号。
function updatePreviewPage(){
  const main = byId("pvMainView").style.display !== "none";
  const hasCard = model.events.some((e) => (e.viewMode ?? 0) === 1);
  const label = byId("pvEventPage");
  label.style.display = (main || !hasCard) ? "none" : "";
  label.textContent = "1/" + Math.max(1, ringPageTotal());
}

// 切换预览视图时，页码与底部提示行也要跟真机一致。
function showPreviewView(which){
  const main = which === "main";
  byId("pvMainView").style.display = main ? "" : "none";
  byId("pvEventView").style.display = main ? "none" : "";
  updatePreviewPage();
  updatePreviewHint();
}

function iconPicker(container, onPick, selected, onAvatarChanged){
  container.innerHTML = "";
  const total = ICONS.length + AVATAR_MAX;

  for(let i = 0; i < total; i++){
    const custom = i >= ICONS.length;
    const slot = i - ICONS.length;
    const uploaded = custom && !!AVATARS[slot];

    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = (i === selected) ? "sel" : "";
    if(custom && !uploaded) btn.classList.add("empty");
    btn.title = iconLabel(i);
    btn.innerHTML = (custom && !uploaded)
      ? '<span class="plus">＋</span>'
      : `<img class="${custom ? "rounded" : ""}" src="${custom ? avatarThumb(AVATARS[slot], AVATAR_PALETTES[slot]) : ICONS[i].data}" alt="">`;
    btn.onclick = () => {
      // 空的自定义槽位：点一下直接选图，省掉"先选中再上传"的两步
      if(custom && !uploaded){
        pickAvatarFile(async (file) => {
          await uploadAvatar(slot, file);
          onPick(i);
          if(onAvatarChanged) await onAvatarChanged();
          toast("头像已存入设备");
        });
        return;
      }
      onPick(i);
    };
    container.appendChild(btn);
  }

  // 选中自定义槽位时补一条操作栏（占满整行，不参与 8 列网格）
  if(selected >= ICONS.length){
    const slot = selected - ICONS.length;
    const bar = document.createElement("div");
    bar.className = "avabar";
    bar.innerHTML =
      `<button type="button" class="ghost" data-act="up">${AVATARS[slot] ? "替换图片" : "上传图片"}</button>`
      + (AVATARS[slot] ? '<button type="button" class="ghost" data-act="clear">清除</button>' : "")
      + '<span class="muted">会压成 40×40，并按"照片像素化强度"那一档（或高级参数）取色后存进去</span>';

    bar.querySelector('[data-act="up"]').onclick = () => pickAvatarFile(async (file) => {
      await uploadAvatar(slot, file);
      onPick(selected);
      if(onAvatarChanged) await onAvatarChanged();
      toast("头像已存入设备");
    });

    const clearBtn = bar.querySelector('[data-act="clear"]');
    if(clearBtn) clearBtn.onclick = async () => {
      try{
        await clearAvatar(slot);
        onPick(0);
        if(onAvatarChanged) await onAvatarChanged();
        toast("已清除，退回内置图标");
      }catch(e){ toast("清除失败：" + e.message); }
    };

    container.appendChild(bar);
  }
}

function renderAll(){
  byId("start").value = model.start;
  byId("blankOff").value = String(model.blankOff ?? 30);
  byId("bleEnabled").value = model.bleEnabled ? "1" : "0";
  byId("nameA").value = model.people[0].name;
  byId("nameB").value = model.people[1].name;
  iconPicker(byId("iconsA"), (i)=>{ model.people[0].icon = i; renderAll(); },
             model.people[0].icon, refreshAvatars);
  iconPicker(byId("iconsB"), (i)=>{ model.people[1].icon = i; renderAll(); },
             model.people[1].icon, refreshAvatars);
  renderEvents();
  renderPreview();
}

const openIcon = new Set();   // 头像选择器处于展开状态的事件下标
const collapsed = new Set();  // 处于"收起"状态的事件卡下标

// 把用户填的文本放进 HTML 属性/文本里。名字与分类都是用户自由输入的,
// 里面带个引号或尖括号就会把这段模板撑坏。
const esc = (s) => String(s ?? "").replace(/[&<>"]/g,
  (c) => ({ "&":"&amp;", "<":"&lt;", ">":"&gt;", '"':"&quot;" }[c]));

// 用户填过的分类名(去重、按首次出现排序),给分类输入框的 datalist 用 ——
// 少手打一遍就少一次把"生日"写成"生日 "那样的错。
function usedCategories(){
  const seen = [];
  model.events.forEach((e) => {
    const c = (e.category || "").trim();
    if(c && !seen.includes(c)) seen.push(c);
  });
  return seen;
}

function updateCategoryOptions(){
  const list = byId("categoryOptions");
  if(!list) return;
  list.innerHTML = usedCategories().map((c) => `<option value="${esc(c)}"></option>`).join("");
}

// 上移/下移只改持久顺序(设备端的显示分组顺序由它决定),不改任何事件的内容。
function moveEvent(idx, delta){
  const to = idx + delta;
  if(to < 0 || to >= model.events.length) return;
  const [moved] = model.events.splice(idx, 1);
  model.events.splice(to, 0, moved);
  remapIndexSets(idx, to);
  renderEvents(); renderPreview();
}

// 两条事件换位后,把"按下标记录"的两套界面状态跟着搬过去。
// 不搬的话,收起/展开的会是换位之后的另一条,看起来像随机跳动。
function remapIndexSets(from, to){
  [openIcon, collapsed].forEach((set) => {
    const hadFrom = set.has(from);
    const hadTo = set.has(to);
    set.delete(from);
    set.delete(to);
    if(hadTo) set.add(from);
    if(hadFrom) set.add(to);
  });
}

function renderEvents(){
  const host = byId("events");
  host.innerHTML = "";
  updateCategoryOptions();
  if(!model.events.length){
    openIcon.clear();
    collapsed.clear();
    host.innerHTML = '<p class="muted">还没有事件，点下面的按钮添加。</p>';
    return;
  }
  model.events.forEach((e, idx) => {
    const open = openIcon.has(idx);
    const isCollapsed = collapsed.has(idx);
    const iconLabelText = iconLabel(e.icon);
    const iconSrcText = iconSrc(e.icon);
    const isLunar = e.kind === 2;
    const lunarMonth = e.lunarMonth ?? 1;
    const lunarDay = e.lunarDay ?? 1;

    const monthOpts = Array.from({length: 12}, (_, i) =>
      `<option value="${i+1}"${(i+1) === lunarMonth ? " selected" : ""}>${LUNAR_MONTHS[i+1]}</option>`).join("");
    // 0 = 月末，专门留给除夕这类节日
    const dayWords = ["月末", "初一","初二","初三","初四","初五","初六","初七","初八","初九","初十",
      "十一","十二","十三","十四","十五","十六","十七","十八","十九","二十",
      "廿一","廿二","廿三","廿四","廿五","廿六","廿七","廿八","廿九","三十"];
    const dayOpts = dayWords.map((w, i) =>
      `<option value="${i}"${i === lunarDay ? " selected" : ""}>${w}</option>`).join("");

    const box = document.createElement("div");
    box.className = "event";
    // 标题上带上名字:收起之后仍要知道这是哪一条,不然只能靠逐条展开找。
    box.innerHTML = `
      <div class="head">
        <span>事件 ${idx+1}${e.name ? " · " + esc(e.name) : ""}${
          e.viewMode === 1 ? ' <span class="muted">单页</span>' : ""}</span>
        <span class="headbtns">
          <button type="button" class="ghost tile" data-move="${idx}" data-delta="-1"
                  title="上移"${idx === 0 ? " disabled" : ""}>↑</button>
          <button type="button" class="ghost tile" data-move="${idx}" data-delta="1"
                  title="下移"${idx === model.events.length - 1 ? " disabled" : ""}>↓</button>
          <button type="button" class="ghost" data-collapse="${idx}">${isCollapsed ? "展开" : "收起"}</button>
          <button type="button" class="ghost" data-remove="${idx}">删除</button>
        </span>
      </div>
      <div class="body"${isCollapsed ? " hidden" : ""}>
      <label>名称</label><input type="text" maxlength="8" data-name="${idx}" value="${esc(e.name)}">
      <label>分类 <span class="muted">留空 = 不分类</span></label>
      <input type="text" maxlength="8" list="categoryOptions" data-category="${idx}"
             value="${esc(e.category)}" placeholder="例如 生日、节日、家人">
      <label>展示</label>
      <select data-view="${idx}">
        <option value="0"${(e.viewMode ?? 0) === 0 ? " selected" : ""}>进列表（一屏 4 条，可按分类分组）</option>
        <option value="1"${e.viewMode === 1 ? " selected" : ""}>单页（自己独占一屏）</option>
      </select>
      <label>重复方式</label>
      <select data-kind="${idx}">
        <option value="0"${e.kind===0?" selected":""}>每年重复（生日 / 节日）</option>
        <option value="1"${e.kind===1?" selected":""}>仅一次（具体纪念日）</option>
        <option value="2"${isLunar?" selected":""}>农历每年（春节 / 中秋）</option>
      </select>
      ${isLunar
        ? `<label>农历日期 <span class="muted">${lunarName(lunarMonth, lunarDay)}</span></label>
           <div class="row">
             <select data-lmonth="${idx}">${monthOpts}</select>
             <select data-lday="${idx}">${dayOpts}</select>
           </div>
           <p class="muted">农历节日请选这项：设备会按农历折算到下一次的公历日期。选“月末”就是除夕那种。</p>`
        : `<label>日期</label><input type="date" data-date="${idx}" value="${e.date}">`}
      <label>头像 <span class="muted">${iconLabelText}</span></label>
      <div class="iconrow">
        <button type="button" class="iconcur" data-toggle="${idx}" title="更换头像">
          <img class="${e.icon >= ICONS.length ? "rounded" : ""}" src="${iconSrcText}" alt="${iconLabelText}"></button>
        <button type="button" class="ghost" data-toggle="${idx}">${open ? "收起" : "更换"}</button>
      </div>
      <div class="icons" data-icons="${idx}"${open ? "" : " hidden"}></div>
      </div>`;
    host.appendChild(box);
    iconPicker(box.querySelector(`[data-icons="${idx}"]`),
      (i)=>{ model.events[idx].icon = i; openIcon.delete(idx); renderEvents(); renderPreview(); },
      e.icon, refreshAvatars);
  });
  host.querySelectorAll("[data-toggle]").forEach(b => b.onclick = () => {
    const i = Number(b.dataset.toggle);
    if(openIcon.has(i)) openIcon.delete(i); else openIcon.add(i);
    renderEvents();
  });
  host.querySelectorAll("[data-collapse]").forEach(b => b.onclick = () => {
    const i = Number(b.dataset.collapse);
    if(collapsed.has(i)) collapsed.delete(i); else collapsed.add(i);
    // 收起时把头像选择器也收掉,否则展开回来会发现它莫名其妙开着。
    openIcon.delete(i);
    renderEvents();
  });
  host.querySelectorAll("[data-move]").forEach(b => b.onclick = () => {
    moveEvent(Number(b.dataset.move), Number(b.dataset.delta));
  });
  host.querySelectorAll("[data-remove]").forEach(b => b.onclick = () => {
    const i = Number(b.dataset.remove);
    model.events.splice(i, 1);
    // 下标会整体前移,两套按下标记录的状态都不再有意义。
    openIcon.clear();
    collapsed.clear();
    renderEvents(); renderPreview();
  });
  host.querySelectorAll("[data-name]").forEach(i => i.oninput = () => {
    model.events[Number(i.dataset.name)].name = i.value; renderPreview();
  });
  host.querySelectorAll("[data-category]").forEach(i => i.oninput = () => {
    model.events[Number(i.dataset.category)].category = i.value;
    // 只刷 datalist,不重建列表 —— 重建会让正在输入的这个框失去焦点。
    updateCategoryOptions();
  });
  host.querySelectorAll("[data-view]").forEach(i => i.onchange = () => {
    const idx = Number(i.dataset.view);
    model.events[idx].viewMode = Number(i.value);
    // 只刷预览的提示行,不重建整张列表 —— 重建会把刚操作的这一条挤走。
    renderPreview();
  });
  host.querySelectorAll("[data-date]").forEach(i => i.onchange = () => {
    model.events[Number(i.dataset.date)].date = i.value; renderPreview();
  });
  host.querySelectorAll("[data-kind]").forEach(i => i.onchange = () => {
    const idx = Number(i.dataset.kind);
    const e = model.events[idx];
    e.kind = Number(i.value);
    if(e.kind === 2){
      // 切到农历时补默认值，并保证 date 字段不再被当成农历来源
      if(e.lunarMonth == null) e.lunarMonth = 1;
      if(e.lunarDay == null) e.lunarDay = 1;
    }
    // 日期控件会随 kind 换形态，必须整体重建
    renderEvents(); renderPreview();
  });
  host.querySelectorAll("[data-lmonth]").forEach(i => i.onchange = () => {
    const e = model.events[Number(i.dataset.lmonth)];
    e.lunarMonth = Number(i.value);
    renderEvents(); renderPreview();
  });
  host.querySelectorAll("[data-lday]").forEach(i => i.onchange = () => {
    const e = model.events[Number(i.dataset.lday)];
    e.lunarDay = Number(i.value);
    renderEvents(); renderPreview();
  });
  byId("eventCount").textContent = model.events.length ? `共 ${model.events.length} 条` : "还没有事件";
}

let lastTime = null;
let lastBattery = null;
function renderTimeAndNet(state){
  lastTime = state.time;
  lastBattery = typeof state.battery === "number" && state.battery >= 0 ? state.battery : null;
  byId("timeText").textContent = state.time.synced ? state.time.text : "未同步";
  const pill = byId("timePill");
  pill.textContent = state.time.synced ? state.time.sourceText : "等待对时";
  pill.className = "pill " + (state.time.synced ? "ok" : "warn");

  const net = state.net;
  const np = byId("netPill");
  np.textContent = net.stateText;
  np.className = "pill " + (net.state === "connected" ? "ok" : (net.ap ? "warn" : ""));
  byId("netIp").textContent = net.ip || "—";
  byId("netSsid").textContent = net.ssid || (net.hasCredentials ? "（正在选网）" : "尚未配置");
  renderSavedList(net);
  byId("netUrl").textContent = net.url || "热点已关闭";
  // 热点空闲关闭后(默认 5 分钟无操作)只能从同一局域网访问,所以两个地址都要给。
  byId("netLanUrl").textContent = net.lanUrl || "未联网";
  byId("apInfo").textContent = net.ap ? `${net.apSsid} / 密码 ${net.apPass}` : "已关闭";
  // 手动关掉的热点不会再自动打开（联网失败时也不会兜底打开），所以要跟"空闲关掉"
  // 区分开：前者是用户决定的，后者一会儿还会自己回来。
  byId("apHint").textContent = net.ap
    ? "5 分钟无操作后自动关闭。"
    : (net.apManualOff ? "已手动关闭，不会再自动打开（联网失败也不会兜底打开）。点“开热点”可以恢复。"
                       : "当前未开启；未配网时开机自动打开，配了网但连不上时也会兜底打开。");
  byId("deviceName").textContent = state.deviceName;
}

async function load(){
  const state = await api("/api/state");
  model = JSON.parse(JSON.stringify(state.config));
  AVATARS = state.avatars || new Array(AVATAR_MAX).fill("");
  AVATAR_PALETTES = state.avatar_palettes || new Array(AVATAR_MAX).fill("");
  byId("deviceName").textContent = state.deviceName;
  // 先吃时间与电量，预览里的对时文案和右上角电量才不会先渲染成占位符。
  renderTimeAndNet(state);
  renderAll();
  // 高级参数的档位/滑块对齐一次（AVATARS 到位后才谈得上"拿现有头像当预览源"）。
  advSyncLabels();
  advPreviewFromAvatar();
}

/* ---------- 已保存的网络 ---------- */

// 列表内容全部来自设备(/api/state 的 net.saved)，网页不自己维护一份 ——
// 同时开两个页面、或者从串口删掉一个，两边就不会各说各话。
//
// 一律用 textContent 拼，**不要用 innerHTML**：SSID 是空口上谁都能写的字符串，
// 把它当 HTML 插进页面等于让一个恶意的热点名字往后台页里注入脚本。
function renderSavedList(net){
  const list = byId("savedList");
  list.innerHTML = "";
  const saved = net.saved || [];

  if(!saved.length){
    const li = document.createElement("li");
    li.className = "muted";
    li.textContent = "还没保存任何网络，用下面的表单添加（最多 5 个）";
    list.appendChild(li);
    return;
  }

  saved.forEach((item, index) => {
    const li = document.createElement("li");
    const name = document.createElement("span");
    name.textContent = `${index + 1}. ${item.ssid}`;
    li.appendChild(name);
    if(item.current){
      const cur = document.createElement("span");
      cur.className = "cur";
      cur.textContent = "当前";
      li.appendChild(cur);
    }
    const del = document.createElement("button");
    del.type = "button";
    del.className = "ghost";
    del.textContent = "删除";
    del.onclick = () => removeSaved(item.ssid);
    li.appendChild(del);
    list.appendChild(li);
  });
}

async function removeSaved(ssid){
  if(!confirm(`删除已保存的「${ssid}」？删掉之后设备不会再自动连它。`)) return;
  try{
    await api("/api/wifi/delete", {method:"POST", body: JSON.stringify({ssid})});
    toast("已删除 " + ssid);
    setTimeout(()=>load().catch(()=>{}), 1200);
  }catch(e){ toast("删除失败：" + e.message); }
}

// 只刷自定义头像(不碰 model，免得把用户没保存的编辑冲掉)
async function refreshAvatars(){
  const state = await api("/api/state");
  AVATARS = state.avatars || AVATARS;
  AVATAR_PALETTES = state.avatar_palettes || AVATAR_PALETTES;
  renderAll();
}

async function save(){
  await api("/api/config", {method:"POST", body: JSON.stringify(model)});
  toast("已保存到设备");
  await load();
}

byId("save").onclick = () => save().catch(e => toast("保存失败：" + e.message));
byId("reload").onclick = () => load().then(()=>toast("已重新载入")).catch(e=>toast(e.message));
// 与设备端 LOVE_EVENT_MAX 一致(24):设备记录一次要能塞进 NVS blob,也要让
// 控制台/网页两个任务各自的 4KB、6KB 栈放得下一个 love_config_t。
const EVENT_MAX = 24;

byId("addEvent").onclick = () => {
  if(model.events.length >= EVENT_MAX){ toast(`最多 ${EVENT_MAX} 条事件`); return; }
  const today = currentToday();
  const iso = today.y + "-" + String(today.m).padStart(2,"0") + "-" + String(today.d).padStart(2,"0");
  // category 显式给空串:让"新加的这条确实没有分类",而不是靠后端按同下标保留旧值。
  model.events.push({name:"新的纪念日", icon:6, kind:0, date:iso, category:"", viewMode:0});
  renderEvents(); renderPreview();
};
byId("start").onchange = (e) => { model.start = e.target.value; renderPreview(); };
byId("blankOff").onchange = (e) => { model.blankOff = Number(e.target.value); };
byId("bleEnabled").onchange = (e) => { model.bleEnabled = e.target.value === "1"; };
byId("nameA").oninput = (e) => { model.people[0].name = e.target.value; renderPreview(); };
byId("nameB").oninput = (e) => { model.people[1].name = e.target.value; renderPreview(); };

byId("syncTime").onclick = async () => {
  const epoch = Math.floor(Date.now()/1000);
  try{
    await api("/api/time", {method:"POST", body: JSON.stringify({epoch})});
    toast("已用手机时间对时");
    const state = await api("/api/state"); renderTimeAndNet(state); renderPreview();
  }catch(e){ toast("对时失败：" + e.message); }
};
byId("scanBtn").onclick = async () => {
  toast("正在扫描…");
  try{
    const data = await api("/api/scan");
    const list = byId("scanList");
    list.innerHTML = "";
    if(!data.aps.length){ list.innerHTML = '<li class="muted">没有扫描到网络</li>'; return; }
    data.aps.forEach(ap => {
      const li = document.createElement("li");
      // 用 textContent 而不是 innerHTML:SSID 来自空口,谁都能取成一个带尖括号的名字。
      const name = document.createElement("span");
      name.textContent = (ap.ssid || "(隐藏)") + (ap.secure ? " 🔒" : "");
      li.appendChild(name);
      const signal = document.createElement("span");
      signal.textContent = `${ap.rssi} dBm`;
      li.appendChild(signal);
      li.onclick = () => { byId("wifiSsid").value = ap.ssid; toast("已填入 " + ap.ssid); };
      list.appendChild(li);
    });
    toast(`扫描完成，共 ${data.aps.length} 个`);
  }catch(e){ toast("扫描失败：" + e.message); }
};
byId("wifiSave").onclick = async () => {
  const ssid = byId("wifiSsid").value.trim();
  if(!ssid){ toast("请填写 Wi-Fi 名称"); return; }
  try{
    await api("/api/wifi", {method:"POST", body: JSON.stringify({ssid, pass: byId("wifiPass").value})});
    byId("wifiPass").value = "";
    toast("已保存，正在连接（状态会稍后刷新）");
    setTimeout(()=>load().catch(()=>{}), 2500);
  }catch(e){ toast("保存失败：" + e.message); }
};
byId("wifiClear").onclick = async () => {
  try{ await api("/api/wifi/clear", {method:"POST", body:"{}"});
       toast("已清除凭据，热点已打开"); setTimeout(()=>load().catch(()=>{}), 1500);
  }catch(e){ toast("清除失败：" + e.message); }
};
byId("apOn").onclick = async () => {
  try{ await api("/api/ap", {method:"POST", body: JSON.stringify({on:true})});
       toast("热点已打开，5 分钟无操作自动关闭"); setTimeout(()=>load().catch(()=>{}), 1200);
  }catch(e){ toast("操作失败：" + e.message); }
};
byId("apOff").onclick = async () => {
  try{ await api("/api/ap", {method:"POST", body: JSON.stringify({on:false})});
       toast("热点已关闭，不会再自动打开"); setTimeout(()=>load().catch(()=>{}), 1200);
  }catch(e){ toast("操作失败：" + e.message); }
};
/* ---------- 高级参数的事件 ---------- */
// 第一个人头像下面的入口：展开共用的高级参数面板并滚过去。面板只有一份，这里只翻它的
// open 状态，不复刻 DOM（复制第二份会让 advSyncLabels 认的两套 id 分叉）。
byId("advOpenA").onclick = () => {
  byId("advBox").open = true;
  byId("advBox").scrollIntoView({ behavior: "smooth", block: "start" });
};
// 选档位 = 把预设抄进五个旋钮；动任一旋钮 = 变成"自定义"档。
byId("avatarStrength").onchange = (e) => {
  advApplyPreset(e.target.value);
  if (e.target.value === "custom") byId("advBox").open = true;
};
byId("advColors").oninput = advFromInputs;
byId("advPalette").onchange = advFromInputs;
byId("advSample").onchange = advFromInputs;
byId("advAssign").onchange = advFromInputs;
byId("advClean").oninput = advFromInputs;
// 调参的闭环：改滑块 -> 直接重传刚才那张（参数就是滑块当前值）。
byId("advReupload").onclick = async () => {
  if (!lastAvatarUpload){ toast("还没选过照片"); return; }
  try{
    await uploadAvatar(lastAvatarUpload.slot, lastAvatarUpload.file);
    toast("已按当前参数重新上传");
  }catch(e){ toast("重新上传失败：" + e.message); }
};
byId("advReset").onclick = () => {
  avatarParams = Object.assign({}, ADV_DEFAULTS);
  byId("avatarStrength").value = AVATAR_PRESET_DEFAULT;
  advSyncLabels();
  advSchedulePreview();
};
// 展开面板时才准备预览（含"拿现有头像当源"那条路），不在页面加载时就白算一遍。
byId("advBox").ontoggle = () => {
  advPreviewFromAvatar();
  advSchedulePreview();
};

byId("pvMain").onclick = () => showPreviewView("main");
byId("pvEvent").onclick = () => showPreviewView("event");

load().catch(e => toast("读取设备状态失败：" + e.message));
setInterval(async () => {
  try{ renderTimeAndNet(await api("/api/state")); renderPreview(); }catch(e){}
}, 10000);
