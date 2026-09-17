// assets/web/admin.js —— 后台网页脚本,由设备以 /admin.js 单独返回。
// 图标是内联的 data URI(一共两千多字节)。不要改回 /icon/N.png 那种独立资源:
// 一次页面加载会多出十六个并发请求,把设备那点堆压到 Wi-Fi 驱动发不出帧。
const ICONS = __ICONS_JSON__;
// 设备端 4bpp 自定义头像用的 16 色调色板,顺序即索引顺序,必须与设备一致。
const PALETTE = __PALETTE_JSON__;
const AVA = 40;                       // 与设备端 LOVE_ICON_PX 一致
const AVATAR_BYTES = AVA * AVA / 2;   // 40x40 4bpp = 800 字节
const AVATAR_MAX = 4;
const TRANSPARENT_PX = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";

const byId = (id) => document.getElementById(id);

// 设备回传的自定义头像(4bpp base64,每个槽位一个,空串表示没上传)
let AVATARS = new Array(AVATAR_MAX).fill("");

let model = { start:"", blankOff:30, people:[{name:"",icon:0},{name:"",icon:1}], events:[] };

function toast(msg){ const t = byId("toast"); t.textContent = msg; t.classList.add("show");
  clearTimeout(t._h); t._h = setTimeout(()=>t.classList.remove("show"), 2200); }

async function api(path, options){
  const res = await fetch(path, Object.assign({headers:{"Content-Type":"application/json"}}, options));
  if(!res.ok) throw new Error(await res.text() || res.statusText);
  return res.headers.get("Content-Type")?.includes("json") ? res.json() : res.text();
}

/* ---------- 自定义头像：网页压缩 -> 设备存 4bpp ---------- */

// 在设备那张 16 色调色板里找最接近的颜色(欧氏距离)。像素风素材本来就只有这 16 色,
// 所以量化后观感一致;照片会退化成这 16 色的网点图,这正是"像素头像"要的效果。
function nearestPaletteIndex(r, g, b){
  let best = 0, bestD = Infinity;
  for(let i = 0; i < PALETTE.length; i++){
    const p = PALETTE[i];
    const d = (r - p[0]) ** 2 + (g - p[1]) ** 2 + (b - p[2]) ** 2;
    if(d < bestD){ bestD = d; best = i; }
  }
  return best;
}

// 选中的图片 -> 40x40、量化到 16 色、打成 4bpp(每字节两个像素,高半字节在前)。
// 先居中裁成正方形再缩放,避免把脸拉扁。
//
// 用 <img> 而不是 createImageBitmap 解码:iPhone 相册默认是 HEIC,Safari 的
// createImageBitmap 对它的支持比 <img> 窄得多;走 <img> 就是走浏览器自己的
// 图像管线,手机直接拍的照片也能选。解码失败时给一句人话,不要把原始异常抛到
// toast 里。
async function compressAvatar(file){
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
    const canvas = document.createElement("canvas");
    canvas.width = AVA; canvas.height = AVA;
    const ctx = canvas.getContext("2d", { willReadFrequently: true });
    ctx.drawImage(bitmap, (w - side) / 2, (h - side) / 2, side, side, 0, 0, AVA, AVA);
    const px = ctx.getImageData(0, 0, AVA, AVA).data;

    const out = new Uint8Array(AVATAR_BYTES);
    for(let i = 0; i < AVA * AVA; i++){
      // 近乎透明的像素直接给白色(索引 1 = W),免得压出脏边
      const idx = px[i*4 + 3] >= 128
        ? nearestPaletteIndex(px[i*4], px[i*4 + 1], px[i*4 + 2])
        : 1;
      if(i % 2 === 0) out[i >> 1] = idx << 4;
      else out[i >> 1] |= idx;
    }
    return out;
  } finally {
    URL.revokeObjectURL(url);
  }
}

// 设备回传的 4bpp base64 -> 可直接显示的 data URI(选择器里的缩略图)
function avatarThumb(base64){
  if(!base64) return TRANSPARENT_PX;
  const bin = atob(base64);
  if(bin.length < AVATAR_BYTES) return TRANSPARENT_PX;

  const canvas = document.createElement("canvas");
  canvas.width = AVA; canvas.height = AVA;
  const ctx = canvas.getContext("2d");
  const img = ctx.createImageData(AVA, AVA);
  for(let i = 0; i < AVA * AVA; i++){
    const byte = bin.charCodeAt(i >> 1);
    const idx = (i % 2 === 0) ? (byte >> 4) : (byte & 0x0F);
    const p = PALETTE[idx] || [0, 0, 0];
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
  const data = AVATARS[idx - ICONS.length];
  return data ? avatarThumb(data) : ICONS[0].data;
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
  const bytes = await compressAvatar(file);
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
    }catch(e){
      toast("上传失败：" + e.message);
    }
  };
  avatarInput.click();
}

/* 与设备 love_date.c 一致的规则：目标是“下一次发生日”，在一起天数含当天。 */
function daysBetween(a, b){
  const ms = Date.UTC(b.y, b.m-1, b.d) - Date.UTC(a.y, a.m-1, a.d);
  return Math.round(ms / 86400000);
}
function parseDate(text){
  if(!text) return null;
  const p = text.split("-").map(Number);
  if(p.length !== 3 || !p[0]) return null;
  return {y:p[0], m:p[1], d:p[2]};
}
function todayFrom(epoch){
  const d = new Date((epoch + 8*3600) * 1000);
  return {y:d.getUTCFullYear(), m:d.getUTCMonth()+1, d:d.getUTCDate(),
          hh:String(d.getUTCHours()).padStart(2,"0"), mm:String(d.getUTCMinutes()).padStart(2,"0")};
}
function daysInMonth(y,m){ return new Date(Date.UTC(y, m, 0)).getUTCDate(); }
function nextOccurrence(today, month, day){
  const clamp = (y)=>Math.min(day, daysInMonth(y, month));
  let cand = {y:today.y, m:month, d:clamp(today.y)};
  if(daysBetween(today, cand) >= 0) return cand;
  return {y:today.y+1, m:month, d:clamp(today.y+1)};
}

function currentToday(){
  return lastTime && lastTime.synced ? todayFrom(lastTime.epoch) : todayFrom(Math.floor(Date.now()/1000));
}

/* ---------- 农历(与设备端 love_lunar.c 同一张表、同一套算法) ---------- */

// 不用浏览器的 Intl 中国农历：实测 18 个年份里有 2 个(2027、2030)与权威日期差 ±1 天，
// 拿它当预览依据会出现"网页说 02-07、设备说 02-06"。这里直接用设备端那张表。
const LUNAR = __LUNAR_JSON__;
const LUNAR_INFO = LUNAR.info.map((hex) => parseInt(hex, 16));
const LUNAR_BASE = { y: LUNAR.baseYear, m: LUNAR.baseMonth, d: LUNAR.baseDay };

const dayNumber = (y, m, d) => Math.round(Date.UTC(y, m - 1, d) / 86400000);
const fromDayNumber = (n) => {
  const dt = new Date(n * 86400000);
  return { y: dt.getUTCFullYear(), m: dt.getUTCMonth() + 1, d: dt.getUTCDate() };
};
const lunarInRange = (y) => y >= LUNAR.baseYear && y < LUNAR.baseYear + LUNAR_INFO.length;

function lunarLeapMonth(y){
  return lunarInRange(y) ? (LUNAR_INFO[y - LUNAR.baseYear] & 0x0F) : 0;
}
function lunarPlainMonthDays(y, m){
  return (LUNAR_INFO[y - LUNAR.baseYear] & (1 << (4 + m - 1))) ? 30 : 29;
}
function lunarLeapDays(y){
  return (LUNAR_INFO[y - LUNAR.baseYear] & (1 << 16)) ? 30 : 29;
}
function lunarYearDays(y){
  let days = 0;
  for(let m = 1; m <= 12; m++) days += lunarPlainMonthDays(y, m);
  const leap = lunarLeapMonth(y);
  if(leap) days += lunarLeapDays(y);
  return days;
}

// 农历 (year, month, day) -> 公历；day = 0 表示该月最后一天(除夕那种)。
function lunarToSolar(ly, lm, ld){
  if(!lunarInRange(ly) || lm < 1 || lm > 12) return null;

  const leap = lunarLeapMonth(ly);
  let offset = 0;
  for(let m = 1; m < lm; m++){
    offset += lunarPlainMonthDays(ly, m);
    if(leap === m) offset += lunarLeapDays(ly);   // 闰月排在其月之后
  }
  const length = lunarPlainMonthDays(ly, lm);
  const day = (ld === 0) ? length : ld;
  if(day < 1 || day > length) return null;

  let before = 0;
  for(let y = LUNAR.baseYear; y < ly; y++) before += lunarYearDays(y);
  return fromDayNumber(dayNumber(LUNAR_BASE.y, LUNAR_BASE.m, LUNAR_BASE.d)
                       + before + offset + day - 1);
}

// 农历月日在 today 当天或之后的下一次发生日(含今天)。
function nextLunar(today, lm, ld){
  const from = dayNumber(today.y, today.m, today.d);
  let best = null;
  for(let ly = today.y - 1; ly <= today.y + 1; ly++){
    const c = lunarToSolar(ly, lm, ld);
    if(!c) continue;
    const n = dayNumber(c.y, c.m, c.d);
    if(n < from) continue;
    if(!best || n < best.n) best = { n, ...c };
  }
  return best ? { y: best.y, m: best.m, d: best.d } : null;
}

const LUNAR_MONTHS = ["", "正月","二月","三月","四月","五月","六月",
                      "七月","八月","九月","十月","冬月","腊月"];

// 与设备端 love_lunar_format 一致:初八 / 十五 / 廿二 / 三十 / 月末
function lunarName(month, day){
  if(month < 1 || month > 12) return "农历";
  if(day === 0) return `${LUNAR_MONTHS[month]}最后一天`;
  const TENS = ["初", "十", "廿", "三"], UNITS = ["十","一","二","三","四","五","六","七","八","九"];
  let dayName;
  if(day === 10) dayName = "初十";
  else if(day === 20) dayName = "二十";
  else if(day === 30) dayName = "三十";
  else dayName = TENS[Math.floor(day / 10)] + UNITS[day % 10];
  return LUNAR_MONTHS[month] + dayName;
}

function renderPreview(){
  const today = currentToday();
  const start = parseDate(model.start);
  byId("pvDays").textContent = start ? (daysBetween(start, today) + 1) : "--";
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

  const e = model.events[0];
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
  updatePreviewHint();
}

// 底部提示行要跟真机一致:农历事件的卡片提示不同,列表模式下主屏的上/下是"进列表"。
// 单独成函数是因为它同时依赖"当前预览哪个屏"和"展示模式",两边都要能触发刷新。
function updatePreviewHint(){
  const main = byId("pvMainView").style.display !== "none";
  const lunarFirst = model.events[0] && model.events[0].kind === 2;
  byId("pvHint").textContent = main
    ? (model.displayMode === 0 ? "上/下 列表 · 长按确定 设置" : "上/下 切换 · 长按确定 设置")
    : (lunarFirst ? "确定 改农历日期" : "确定 改日期");
}

// 切换预览视图时，页码与底部提示行也要跟真机一致。
function showPreviewView(which){
  const main = which === "main";
  byId("pvMainView").style.display = main ? "" : "none";
  byId("pvEventView").style.display = main ? "none" : "";
  byId("pvEventPage").style.display = main ? "none" : "";
  byId("pvEventPage").textContent = "1/" + model.events.length;
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
      : `<img class="${custom ? "rounded" : ""}" src="${custom ? avatarThumb(AVATARS[slot]) : ICONS[i].data}" alt="">`;
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
      + '<span class="muted">会压成 40×40、量化到设备那 16 色后存进去</span>';

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
  // 展示模式:缺字段时按设备的出厂默认(单页)显示,别让选择器停在一个空值上。
  byId("displayMode").value = String(model.displayMode ?? 1);
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
        <span>事件 ${idx+1}${e.name ? " · " + esc(e.name) : ""}</span>
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
  byId("netSsid").textContent = net.ssid || "尚未配置";
  byId("netUrl").textContent = net.url || "热点已关闭";
  // 热点空闲关闭后(默认 5 分钟无操作)只能从同一局域网访问,所以两个地址都要给。
  byId("netLanUrl").textContent = net.lanUrl || "未联网";
  byId("apInfo").textContent = net.ap ? `${net.apSsid} / 密码 ${net.apPass}` : "已关闭";
  byId("deviceName").textContent = state.deviceName;
}

async function load(){
  const state = await api("/api/state");
  model = JSON.parse(JSON.stringify(state.config));
  AVATARS = state.avatars || new Array(AVATAR_MAX).fill("");
  byId("deviceName").textContent = state.deviceName;
  // 先吃时间与电量，预览里的对时文案和右上角电量才不会先渲染成占位符。
  renderTimeAndNet(state);
  renderAll();
}

// 只刷自定义头像(不碰 model，免得把用户没保存的编辑冲掉)
async function refreshAvatars(){
  const state = await api("/api/state");
  AVATARS = state.avatars || AVATARS;
  renderAll();
}

async function save(){
  await api("/api/config", {method:"POST", body: JSON.stringify(model)});
  toast("已保存到设备");
  await load();
}

byId("save").onclick = () => save().catch(e => toast("保存失败：" + e.message));
byId("reload").onclick = () => load().then(()=>toast("已重新载入")).catch(e=>toast(e.message));
byId("addEvent").onclick = () => {
  if(model.events.length >= 8){ toast("最多 8 条事件"); return; }
  const today = currentToday();
  const iso = today.y + "-" + String(today.m).padStart(2,"0") + "-" + String(today.d).padStart(2,"0");
  // category 显式给空串:让"新加的这条确实没有分类",而不是靠后端按同下标保留旧值。
  model.events.push({name:"新的纪念日", icon:6, kind:0, date:iso, category:""});
  renderEvents(); renderPreview();
};
byId("start").onchange = (e) => { model.start = e.target.value; renderPreview(); };
byId("blankOff").onchange = (e) => { model.blankOff = Number(e.target.value); };
byId("displayMode").onchange = (e) => {
  model.displayMode = Number(e.target.value);
  renderPreview();   // 主屏提示行会跟着模式变
};
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
      li.innerHTML = `<span>${ap.ssid || "(隐藏)"}${ap.secure ? " 🔒" : ""}</span><span>${ap.rssi} dBm</span>`;
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
       toast("热点已关闭"); setTimeout(()=>load().catch(()=>{}), 1200);
  }catch(e){ toast("操作失败：" + e.message); }
};
byId("pvMain").onclick = () => showPreviewView("main");
byId("pvEvent").onclick = () => showPreviewView("event");

load().catch(e => toast("读取设备状态失败：" + e.message));
setInterval(async () => {
  try{ renderTimeAndNet(await api("/api/state")); renderPreview(); }catch(e){}
}, 10000);
