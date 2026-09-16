<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

### love_font_12 / love_font_24 / love_font_36（恋爱倒计时的像素字库）

设备界面用**点阵像素字体**，与像素图标、像素底纹是一套视觉。界面只允许这三种字号。

- 文件（均由 `lv_font_conv` 生成）：
  - `fonts/love_font_12.c`：正文小字，界面提示行、起始日、单位、电量、设置页
  - `fonts/love_font_24.c`：主字号，标题、人像名字、事件名
  - `fonts/love_font_36.c`：只含 `0123456789+-.:/%`，用于天数大数字
- 来源与许可：**方舟像素字体 / Ark Pixel Font** 12px 尺寸、简体中文（zh_cn）比例模式，
  SIL Open Font License 1.1，随附 `fonts/OFL-ark-pixel.txt`；仓库只提交生成结果
  与转换命令，不提交原始 TTF。
- **为什么字号只能是 12 / 24 / 36**：像素字体是在固定的 12px 设计网格上手绘的，
  只有整数倍放大（1×/2×/3×）才能保证每个笔画仍然落在整像素上。实测该字体在
  12/24/36/48 下 `adv_w` 与 `box_w/h` 严格成倍数关系，非整数倍会让笔画粗细不匀、
  失去点阵观感。**不要新增第四种字号**。
- 字符范围：ASCII 0x20–0x7E、常用中文标点、GB2312 一级汉字。
  字符集与字体自身 cmap 求过交集，实际收录 3718 字（该字体缺 175 个一级字，
  例如部分生僻字；缺字会回落到 Montserrat，因此人名里出现生僻字可能显示异常）。
- 生成命令（仓库根目录，`lv_font_conv` 版本 1.5.3）：
  字符集与生成脚本见 `assets/images/` 同级的开发记录；核心命令形如：

  ```bash
  lv_font_conv \
    --font <ark-pixel-12px-proportional-zh_cn.ttf> \
    --symbols "<字符集>" \
    --size 24 --bpp 1 --format lvgl --no-compress \
    --lv-font-name love_font_24 --lv-include lvgl.h \
    --output assets/fonts/love_font_24.c
  ```

  ⚠ 字符集有上万字节，**不要把 `--symbols` 的内容写在 shell 命令行里**：CJK 经过
  中间层会被破坏，实测只会收进个别字符且不报错。正确做法是把字符集读进脚本语言
  的字符串，再以「参数数组」形式调用（`child_process.spawnSync` / `subprocess.run`）。

- 目标放置路径：`assets/fonts/love_font_*.c`，由 `main/CMakeLists.txt` 的
  `target_sources` 编译进 `main` 组件；应用侧用 `LV_FONT_DECLARE` 声明，
  并复制一份可写描述符把 fallback 指向 Montserrat，覆盖缺字与 LVGL 图标。
- 代价：合计约 330 KB Flash（1bpp 未压缩），只读 Flash，不常驻内部 RAM。
- 其他字号请另行生成并登记,不要为了补一个字改用整套 CJK 字库。

### ark12-subset.woff2（后台网页用的同款像素字体）

- 文件：`fonts/ark12-subset.woff2`（约 122 KB），来源与许可同上。
- 用途：后台管理页的设备预览要显示成和真机一样的点阵字形，否则只能对齐坐标、
  看不出像素味。由 `tools/gen_admin_page.py` 与 `tools/preview_admin_page.py`
  以 base64 `data:` URI 内嵌进页面。
- 生成：用 `fonttools` 对同一份 TTF 取与设备端相同的字符集后转 woff2：

  ```bash
  pyftsubset ark-pixel-12px-proportional-zh_cn.ttf \
    --text-file=<字符集文件> --flavor=woff2 --no-hinting --desubroutinize \
    --layout-features='' --output-file=assets/fonts/ark12-subset.woff2
  ```

- 代价：base64 内嵌后页面体积增加约 163 KB，会一并占固件 Flash。


## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

### 恋爱倒计时像素素材（love_pixel_art）

- 源文件：`images/love_pixel_art_gen.py`（8×8 像素掩码 + 16 色调色板，无第三方依赖）。
- 生成结果：
  - `images/love_pixel_art.c` + `main/love_pixel_art.h`：16 个 40×40 ARGB8888
    图标（掩码放大 5 倍，整数倍才不会有半像素）、48×48 RGB565 爱心底纹平铺砖，
    以及 `love_pixel_palette[16]`。
  - `images/web/`：同一份掩码导出的 PNG、`icons.json`/`assets.json` 数据 URI、
    调色板（`palette` 字段）与 `contact-sheet.png` 核对图，供后台网页使用。
- 调色板顺序即自定义头像 4bpp 的索引顺序（`PALETTE_ORDER`），**改动等于让所有
  已上传的自定义头像换色**，所以是显式写死的而不是依赖 dict 顺序。
- 为什么不做圆形头像底座：圆形必然裁掉方形图标的四角，实测 16 个角色里 15 个会掉
  实心像素（猫咪少 186 个、礼物少 284 个，耳朵和边角被切平），保留完整造型更重要。
- 转换步骤：仓库根目录执行 `python3 assets/images/love_pixel_art_gen.py`。
  改掩码后重新运行即可，设备端与网页端会同时更新。
- 目标放置路径：`assets/images/love_pixel_art.c` 由 `main/CMakeLists.txt`
  的 `target_sources` 编译；网页素材经 `tools/gen_admin_page.py` 内联进
  `main/love_admin_page.h`。
- 许可：图标与底纹为本仓库原创的作品，采用本仓库许可证。
- 代价：图标、底纹与调色板合计约 326 KB 源码 / 约 100 KB Flash；
  ARGB8888 图标在绘制时按需转换，不整屏缓存。

### 农历数据表（love_lunar_table）

- 生成器：`tools/gen_lunar_table.py`（需要 `pip install lunarcalendar`），
  同时产出两份：
  - `main/love_lunar_table.h`：设备端用的 C 表，覆盖 2018–2050，每年一个 20 bit 值
    （bit 0..3 闰月号；bit 4..15 正月..腊月天数，1 = 30 天；bit 16 闰月天数）。
  - `images/web/lunar.json`：同一份表给后台网页，两端跑同一套换算。
- **为什么网页也要这张表**：浏览器的 `Intl.DateTimeFormat('zh-CN-u-ca-chinese')`
  实测在 18 个年份里有 2 个（2027 偏 +1 天、2030 偏 −1 天）与公开来源不一致。
  拿它做预览会出现"网页说 02-07、设备说 02-06"。给同一份数据才不会有这种分歧。
- 生成时自带反查：脚本用算出的表逐条推算春节，与写死在脚本里的公开来源日期
  （2018–2035 共 18 个年份）以及 2026 端午/中秋比对，不一致就退出非零、**不写文件**。
  农历算错会静默显示错误日期，比不做更糟，所以这道校验是硬性的。
- 覆盖范围外的年份：设备端返回失败并把界面标成「农历超出范围」，
  不猜一个日期出来。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。
