// assets/web/preview_math.js —— 后台页预览用的倒计时内核(纯函数,不碰 DOM)。
//
// 为什么单独成文件:这些规则**与设备端 C 实现是同一件事**,两边各写一遍就容易各走各的
// —— 网页说"02-07"、设备说"02-06"这种。抽出来以后:
//
//   1. tools/gen_admin_page.py 把它内联进 admin.js(与 avatar_pixel.js 同一套办法),
//      页面上仍然只有 /admin.js 一个请求;
//   2. tests/test_preview_math.mjs 用 node 直接跑**这一份**代码,逐个核对
//      tests/vectors/date_vectors.json —— 那些向量是 tools/gen_date_vectors.py 调设备端
//      C 实现生成的。于是设备端是唯一事实源,页面这边只是它的影子,一漂就红。
//
// 农历表也是同一份(assets/images/web/lunar.json,由 tools/gen_lunar_table.py 生成),
// 所以这里连数据都没有第二个来源。
//
// 不用浏览器的 Intl 中国农历:实测 18 个年份里有 2 个(2027、2030)与权威日期差 ±1 天。

/* 与设备端 love_date.c 一致的规则:目标是"下一次发生日",在一起天数含当天。 */
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

/* ---------- 农历(与设备端 love_lunar.c 同一张表、同一套算法) ---------- */

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

// 农历 (year, month, day) -> 公历;day = 0 表示该月最后一天(除夕那种)。
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

// 与设备端 love_lunar_format 逐字一致:初八 / 十五 / 二十二 / 三十 / 月末。
// 注意「二十X」而不是「廿X」—— 设备那套像素字库没有「廿」的字形,写它会在屏幕上变成
// 方框;这条以前两边写法不同(网页是「廿二」、设备是「二十二」),由向量测试钉住。
function lunarName(month, day){
  if(month < 1 || month > 12) return "农历";
  if(day === 0) return `${LUNAR_MONTHS[month]}最后一天`;
  if(day < 1 || day > 30) return LUNAR_MONTHS[month];
  const TENS = ["初", "十", "二十", "三"], UNITS = ["十","一","二","三","四","五","六","七","八","九"];
  let dayName;
  if(day === 10) dayName = "初十";
  else if(day === 20) dayName = "二十";
  else if(day === 30) dayName = "三十";
  else dayName = TENS[Math.floor(day / 10)] + UNITS[day % 10];
  return LUNAR_MONTHS[month] + dayName;
}
