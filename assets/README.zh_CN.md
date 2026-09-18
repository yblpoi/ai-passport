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

### love_font_12 / love_font_24 / love_font_36（纪念日摆件的像素字库）

设备界面用**点阵像素字体**，与像素图标、像素底纹是一套视觉。界面只允许这三种字号。

- 文件（均由 `lv_font_conv` 生成）：
  - `fonts/love_font_12.c`：正文小字，界面提示行、起始日、单位、电量、设置页
  - `fonts/love_font_24.c`：主字号，标题、人像名字、事件名
  - `fonts/love_font_36.c`：只含 `0123456789+-.:/%`，用于天数大数字
- 来源与许可：**缝合像素字体 / Fusion Pixel Font** 12px 尺寸、简体中文（zh_hans）
  比例模式，SIL Open Font License 1.1，随附 `fonts/OFL-fusion-pixel.txt`。它是方舟
  像素字体（Ark Pixel Font）官方给出的过渡方案：**以方舟像素字体作为基础字形和度量
  参数**，再用其他同尺寸像素字体补足缺口。仍保留 `fonts/OFL-ark-pixel.txt`，因为真正
  被渲染的正是它提供的那些方舟字形。仓库只提交生成结果与转换命令，不提交原始 TTF。
- 使用的缝合像素字体版本：`2026.09.01`，
  `fusion-pixel-font-12px-proportional-ttf-v2026.09.01.zip`，其中的
  `fusion-pixel-12px-proportional-zh_hans.ttf`
  （<https://github.com/TakWolf/fusion-pixel-font/releases>）。
- **为什么字号只能是 12 / 24 / 36**：像素字体是在固定的 12px 设计网格上手绘的，
  只有整数倍放大（1×/2×/3×）才能保证每个笔画仍然落在整像素上。实测该字体在
  12/24/36/48 下 `adv_w` 与 `box_w/h` 严格成倍数关系，非整数倍会让笔画粗细不匀、
  失去点阵观感。**不要新增第四种字号**。
- 字符范围：ASCII 0x20–0x7E、常用中文标点、**完整的 GB2312 一级汉字**（3755）、
  **GB2312 二级汉字**（3008），再加 10 个 GB2312 之外的人名用字（玥 喆 昇 頔 珺 婳 燚 垚 犇 甯），
  共请求 **6910 字**；其中 126 个字源字体里没有字形（都是冷僻的二级字，如 鼗 劐 衮 脔 郄），
  生成器会跳过它们，不报错。**字符集怎么来的**：ASCII 与标点取自原有字符集，汉字由 `gb2312`
  编解码枚举部件得到（一级 = 高位 B0..D7，二级 = 高位 D8..F7），再拼上那 10 个额外用字。
- **为什么从一级扩到二级**：一级字不含 婷 鑫 怡 倩 璇 淼 瑜 瑾 覃 这类**常见人名用字**
  （它们都是二级字，实测婷 = 0xE6C3、鑫 = 0xF6CE；玥 喆 昇 连 GB2312 都没有）。原先只收
  一级字时，名字里出现这些字，LVGL 会走 `LV_USE_FONT_PLACEHOLDER` 画一个整行高的实心白块。
- 早先直接以方舟像素字体为源时，它的 cmap 里缺 172 个一级字，其中包含 然 窗 紧 警 药 恋 缘 热 执
  这类日常用字；改用缝合像素字体重新生成后正好补上（这批字现在也都在）。
- 生成命令（仓库根目录，`lv_font_conv` 版本 1.5.3）：
  字符集与生成脚本见 `assets/images/` 同级的开发记录；核心命令形如：

  ```bash
  lv_font_conv \
    --font <fusion-pixel-12px-proportional-zh_hans.ttf> \
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
- 代价：三套字库合计约 634 KB Flash（12px 170 KB、24px 462 KB、36px 1 KB，map 文件实测；
  1bpp 未压缩），只读 Flash，不常驻内部 RAM。其中新增的 172 个一级字约占 17 KB。
- 其他字号请另行生成并登记,不要为了补一个字改用整套 CJK 字库。
- ⚠ **生成文件的 `Opts:` 注释里存的是生成当时的绝对路径**（`lv_font_conv` 把 `--font` 与
  `--output` 原样写进文件头，属固定行为）。换一台机器或换一个检出目录后，这个路径就会
  与当前仓库位置不一致——**这是溯源注释，不影响字形数据**，字形只由字体文件 + 符号集 +
  字号 + bpp 决定。要用本节的命令重新生成来刷新它，**不要手工编辑生成文件**；
  符号集可以直接从现有文件的 `Opts:` 行原样取回，不必重算。

### ark12-subset.woff2（历史遗留，后台网页已不再使用）

- 文件：`fonts/ark12-subset.woff2`（约 122 KB），来源与许可同上。
- **当前没有任何代码引用它。** 后台网页的设备预览改用浏览器自己的系统字体了。
- 曾经的做法：把这个子集内嵌进页面，让预览和真机是同一套点阵字形。放弃的原因
  是体积——3,891 个字形（ASCII 95 + 中日韩标点约 40 + GB2312 一级字 3,755）
  平均 32 字节/字，而页面自己的固定文案只用到 475 个汉字，为了"任意姓名也能
  显示像素字形"付了二十倍于页面本体的传输量。改成独立路由 + 一小时缓存后仍嫌重，
  于是整个去掉：预览的坐标、字号、行高仍然精确（按 240×320 逻辑像素摆位），
  只是字形变成矢量字体。
- 想恢复时按下面的命令重新生成（源 TTF 见上一节的获取方式，本机没有留存）：

  ```bash
  pyftsubset ark-pixel-12px-proportional-zh_cn.ttf \
    --text-file=<字符集文件> --flavor=woff2 --no-hinting --desubroutinize \
    --layout-features='' --output-file=assets/fonts/ark12-subset.woff2
  ```

  字符集文件可以从 `assets/fonts/love_font_12.c` 头部那行 `Opts: --symbols ...`
  原样取回。若要重新启用，记得同时改 `tools/gen_admin_page.py`（加回字体输入与
  字节输出）、`main/love_httpd.c`（加回 `/font.woff2` 路由）和
  `assets/web/admin.css`（加回 `@font-face`）。
- 代价：留在仓库里占约 122 KB（**不进固件 Flash**）。


## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 嵌入中英文 `docs/README.md` 的硬件概览图。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

### 纪念日摆件像素素材（love_pixel_art）

- 素材源：`images/emoji/`——16 张 Twemoji 图形的 72×72 PNG（CC-BY 4.0，许可原文见
  `LICENSE-GRAPHICS.txt`；`manifest.json` 钉住上游 tag v16.0.0、文件名、码位与每张的
  sha256）。`images/fetch_emoji.py` 负责抓取（固定 tag、逐个校验 sha256，不符即非零
  退出），是维护脚本，不参与构建。
- 从素材到图标：`images/love_pixel_art_gen.py`（无第三方依赖）裁掉每张图的透明边，
  **等比**缩进 20×20 的逻辑像素并居中，再整数倍放大 2 倍到屏上的 40px——每个逻辑像素
  占 2×2 个屏幕像素，像素结构才看得出来。每格取它覆盖的源像素的 **alpha 加权主导色**
  （不是面积平均：平均会造出源图没有的中间色，等于把像素风糊成低分辨率矢量）。等比
  不是偏好：裁边后的框从 38×72（气球）到 72×72（蛋糕），逐轴填满会把气球拉宽近一倍。
- 这条路径替换掉了 2026-09-18 那批**手绘掩码**：那批没法可靠地做出来——设计稿来自
  AI 生图工具，其"像素"并不落在真实栅格上、没法降采样成这个格式，每次修改都要手工排到
  8×8 栅格上。现在的像素风来自素材本身的形状，再采样到真实栅格。
- **图标顺序绝不能改**：`LOVE_ICON_*` 的下标存在用户配置里，换序等于把用户的图标换掉。
  槽位名：9 号仍叫 `LOVE_ICON_MOON`（月饼本来就是 moon cake，不算误导）；
  14 号已从 `LOVE_ICON_LEAF` 改成 `LOVE_ICON_LOVING`——它现在是带爱心的笑脸，
  再叫叶子只会误导下一个读代码的人。改名不动任何设备：配置里存的是**下标**。
- 生成结果：
  - `images/love_pixel_art.c` + `main/love_pixel_art.h`：16 个 40×40 的 4bpp
    索引（I4）图标（20×20 逻辑像素放大 2 倍）、48×48 RGB565 爱心底纹平铺砖，
    以及 `love_pixel_palette[16]`。
  - **每张图标自带**一张 16 色调色板：16 张图用色的并集远超 16 个槽位，而
    `lv_bin_decoder` 的约定本来就是调色板位于每张图的数据头部。哪张图需要超过 15 种
    不透明色，`per_icon_palette()` 会直接报错而不是悄悄丢色（实测用得最多的是
    圣诞树，8 色）。
  - 图标是 I4 而不是 ARGB8888：调色板 16 色内嵌在每个图标数据头部（`lv_color32_t`
    内存顺序 B,G,R,A），其后是每字节 2 像素、高半字节在前的索引。这是
    `lv_bin_decoder` 对 `LV_IMAGE_SRC_VARIABLE` + 索引格式的约定，所以不需要开
    `LV_BIN_DECODER_RAM_LOAD`，绘制时按行按需转换。图标自带调色板的索引 0 固定为
    透明（图标是透明背景的图形），与下面那张不含透明项的头像调色板是两张表。
  - `images/web/`：同一批网格导出的 PNG、`assets.json`（底纹与图标的数据 URI 加
    `palette` 字段——后台网页只内联这一个文件，所以没有第二份图标清单）、
    `contact-sheet.png` 与 3 倍放大棋盘底的 `contact-sheet-zoom.png` 核对图。
- 调色板顺序即自定义头像 4bpp 的索引顺序（`PALETTE_ORDER`），**改动等于让所有
  已上传的自定义头像换色**，所以是显式写死的而不是依赖 dict 顺序。
- 头像圆角：半径 **4px**，**只作用于自定义头像**（上传的照片，满幅方角）；
  内置图标刻意不做。判定只有一份实现——`main/ui_pixel_math.c` 的
  `ui_pixel_corner_cut()`，有主机测试钉住**四角互为镜像**（曾经用"负数当哨兵"写错过一次，
  结果设备上只有右下角被切）。**圆形底座仍然不做**，理由同理：内置图标是图形不是
  方块牌，圆形会切到伸到边缘的那些。**为什么不给内置图标加圆角**：它们是透明背景的
  **图形**而非方块照片；实测 16 张生成图里只有猫咪的两个上角触到画布边缘，4px 圆角
  只会切到它、其余 15 张毫无变化——这条规则就只会作用于唯一需要注意边角的那个图标。
  图形类图标只有"保持原样"和"被切"两种选择，这里选保持原样，圆角规则只属于
  **自定义头像**（满幅照片）。
  两端保持一致：网页端 `assets/web/admin.css` 只给自定义头像的 `img.rounded` 加圆角，
  设备端 custom 头像在打包成 I4 时把角落改指到透明索引
  （头像调色板没有透明项），复用的正是同一个 `ui_pixel_corner_cut()`。
- 转换步骤：仓库根目录执行 `python3 assets/images/love_pixel_art_gen.py`。它读
  `images/emoji/*.png`，换图标就是在生成器的 `EMOJI` 列表里改（名字、标签、码位）；
  要换素材本身还要同步 `images/fetch_emoji.py` 并重新 vendored。之后**必须重跑**
  `python3 tools/gen_admin_page.py`，把新的网页素材折进 `main/love_admin_page.h` 与
  `main/love_web_assets.h`。
- 目标放置路径：`assets/images/love_pixel_art.c` 由 `main/CMakeLists.txt`
  的 `target_sources` 编译；网页素材经 `tools/gen_admin_page.py` 写进
  `main/love_web_assets.h`（底纹 PNG 与页面图标 PNG，分别由 `/bg.png`、
  `/favicon.ico`、`/apple-touch-icon*.png` 返回）与
  `main/love_admin_page.h`（HTML/CSS/JS，由 `/`、`/admin.css`、`/admin.js` 分别返回）。
- 网页端图标**内联在 `admin.js` 里**（data URI），不做成 `/icon/N.png`：
  十六个图标的 PNG 合计 3,949 字节（data URI 5,644 字节），拆成独立请求会让一次
  页面加载多开十几条连接，把设备那点堆压到 Wi-Fi 驱动分不到发送帧。曾经让页面
  打不开的是 167 KB 的像素字体，和图标无关，那个已经删掉了。
- 许可：图标是 **Twemoji 图形**（https://github.com/jdecked/twemoji，tag v16.0.0），
  按上文做像素化之后属于**衍生作品**，仍受 **CC-BY 4.0** 约束——许可原文见
  `images/emoji/LICENSE-GRAPHICS.txt`，必须随素材一起保留。爱心底纹为本仓库原创的
  作品，采用本仓库许可证。
- 代价：图标、底纹与调色板合计约 112 KB 源码 / 18 KB Flash（图标 13.8 KB，
  即 16 张 864 字节的 I4 图；底纹 4.6 KB）；I4 图标在绘制时按行转换，不整屏缓存。

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
