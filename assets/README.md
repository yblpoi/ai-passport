<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

### love_font_12 / love_font_24 / love_font_36 (pixel fonts for the commemorative-day display)

The device UI uses a **bitmap pixel font** so it matches the pixel icons and the
pixel heart wallpaper. Only these three sizes are allowed.

- Files (all generated with `lv_font_conv`):
  - `fonts/love_font_12.c` — small body text: hints, start date, unit, battery,
    settings rows
  - `fonts/love_font_24.c` — main text: title, person names, event names
  - `fonts/love_font_36.c` — digits and `+-.:/%` only, used for the big day count
- Source and license: **Fusion Pixel Font** 12 px size, Simplified Chinese
  (zh_hans), proportional — SIL Open Font License 1.1, shipped as
  `fonts/OFL-fusion-pixel.txt`. Fusion Pixel Font is the Ark Pixel Font project's
  own transitional build: it takes Ark Pixel as the base glyphs and metrics and
  fills the gaps from other same-size pixel fonts. `fonts/OFL-ark-pixel.txt` is
  kept because the Ark Pixel glyphs it is built from are still what gets
  rendered. The repository commits only the generated files and the conversion
  command, not the TTF.
- Fusion Pixel Font release used: `2026.09.01`,
  `fusion-pixel-font-12px-proportional-ttf-v2026.09.01.zip`, file
  `fusion-pixel-12px-proportional-zh_hans.ttf`
  (<https://github.com/TakWolf/fusion-pixel-font/releases>).
- **Why only 12 / 24 / 36**: the font is hand-drawn on a fixed 12 px design grid.
  Only integer multiples (1×/2×/3×) keep every stroke on whole pixels. Measured
  `adv_w` and `box_w/h` scale exactly at 12/24/36/48; a non-integer factor makes
  stroke widths uneven and destroys the dot-matrix look. **Do not add a fourth
  size.**
- Character range: ASCII 0x20–0x7E, common CJK punctuation, and the complete
  GB2312 level-1 Han set — **3890 characters**. An earlier build took the font
  straight from Ark Pixel Font, whose cmap is missing 172 level-1 characters,
  including everyday ones such as the characters for "hot", "execute", "love",
  "fate", "however", "window", "tight", "police" and "medicine". Those had no
  glyph anywhere, so LVGL drew placeholder boxes; a custom name containing one
  would have shown a box too. Regenerating from Fusion Pixel Font adds exactly
  those 172 glyphs and leaves all 3718 existing glyphs byte-identical, so no text
  moved or changed shape.
- Conversion command (repository root, `lv_font_conv` 1.5.3):

  ```bash
  lv_font_conv \
    --font <fusion-pixel-12px-proportional-zh_hans.ttf> \
    --symbols "<character set>" \
    --size 24 --bpp 1 --format lvgl --no-compress \
    --lv-font-name love_font_24 --lv-include lvgl.h \
    --output assets/fonts/love_font_24.c
  ```

  ⚠ The character set is tens of thousands of bytes. **Never put the `--symbols`
  payload on a shell command line**: CJK gets mangled by intermediate layers and
  only a handful of characters survive, with no error reported. Read the set into
  a script string and invoke with an argument array instead
  (`child_process.spawnSync` / `subprocess.run`).

- Destination: `assets/fonts/love_font_*.c`, compiled into the `main` component
  through `target_sources` in `main/CMakeLists.txt`. The application uses
  `LV_FONT_DECLARE` and keeps writable copies whose fallback points at Montserrat
  to cover missing glyphs and LVGL symbols.
- Cost: about 363 KB of Flash altogether (12 px 98 KB, 24 px 264 KB, 36 px 1 KB;
  1 bpp, uncompressed), read-only, not resident in internal RAM. The 172 added
  level-1 glyphs account for roughly 17 KB of that.
- Generate and register additional sizes separately instead of switching to a
  full CJK family to add a single character.

### ark12-subset.woff2 (kept for reference; the admin page no longer uses it)

- File: `fonts/ark12-subset.woff2` (about 122 KB), same source and license.
- **Nothing references it today.** The admin page preview now uses the browser's
  own system font.
- What it used to do: embed this subset so the preview showed the same dot-matrix
  letterforms as the real device. It was dropped for size — 3,891 glyphs
  (95 ASCII, ~40 CJK punctuation, 3,755 GB2312 level-1) average 32 bytes each,
  while the page's own fixed text uses only 475 Chinese characters. Carrying the
  full set so that *any* typed name would render in pixel form cost twenty times
  the rest of the page. Serving it as its own route with an hour of caching was
  still too heavy, so it is gone: the preview's coordinates, font sizes and line
  heights are still exact (laid out on the device's 240×320 logical pixels), only
  the letterforms are now vector.
- To bring it back, regenerate it with the command below (the source TTF is not
  stored in this repository; see the previous section for how to obtain it):

  ```bash
  pyftsubset ark-pixel-12px-proportional-zh_cn.ttf \
    --text-file=<charset file> --flavor=woff2 --no-hinting --desubroutinize \
    --layout-features='' --output-file=assets/fonts/ark12-subset.woff2
  ```

  The charset file can be recovered verbatim from the `Opts: --symbols ...` line
  at the top of `assets/fonts/love_font_12.c`. Re-enabling it also means updating
  `tools/gen_admin_page.py` (font input and byte output),
  `main/love_httpd.c` (the `/font.woff2` route) and `assets/web/admin.css`
  (the `@font-face` rule).
- Cost: about 122 KB in the repository. It is **not** in firmware Flash.

## Images

Store reusable source images and generated display assets in `images/`.

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

### Commemorative-day pixel art (love_pixel_art)

- Source: `images/love_pixel_art_gen.py` (8×8 pixel masks plus a palette, no
  third-party dependencies).
- Generated output:
  - `images/love_pixel_art.c` and `main/love_pixel_art.h`: sixteen 40×40 4 bpp
    indexed (I4) icons (masks scaled 5× — only integer factors avoid half pixels),
    a 48×48 RGB565 heart background tile, and `love_pixel_palette[16]`.
  - The icons are I4 rather than ARGB8888: a 16-colour palette is embedded at the
    head of each icon's data (as `lv_color32_t`, memory order B,G,R,A), followed by
    the indices, two pixels per byte, high nibble first. That is the
    `lv_bin_decoder` convention for `LV_IMAGE_SRC_VARIABLE` plus an indexed format,
    so `LV_BIN_DECODER_RAM_LOAD` is not needed and rows are converted on demand
    while drawing. Index 0 of the per-icon palette is fixed to transparent (the
    icons are hollow), which is a separate table from the avatar palette below.
  - `images/web/`: PNGs exported from the same masks, `icons.json`/`assets.json`
    data URIs, the palette (`palette` field), and `contact-sheet.png` for manual
    review, all used by the admin page.
- The palette order is the 4 bpp index order for uploaded custom avatars
  (`PALETTE_ORDER`). **Changing it recolors every avatar already uploaded**, so it
  is written out explicitly rather than relying on dict order.
- Avatar corner rounding: radius **4 px**, set by `ICON_CORNER_RADIUS`. A **circular**
  chip is still rejected: measured, a circle clips solid pixels off 15 of the 16
  characters (the cat loses 186, the gift 284 — ears and corners get flattened).
  A 4 px rounding is far milder: the four corners clip 17 canvas pixels in total,
  8 of the 16 icons already have empty corners (they are shapes on a transparent
  background, not square tiles), and only 64 pixels of real artwork are removed
  across all sixteen — 12 each on the star, cake and gift, 8 on the leaf, 5 each
  on the cat, dog, bear and fox. It is what keeps an uploaded photo from looking
  hard-edged.
  The radius must match in three places: this file, `AVATAR_CORNER_RADIUS` in
  `main/love_app.c` (the custom-avatar palette has no transparent entry, so the
  corners can only be masked to alpha 0 while decoding to ARGB8888), and
  `assets/web/admin.css`. Built-in icons instead get their corner pixels set to
  transparent at generation time (palette index 0 is transparent), which keeps the
  I4 data, the exported PNGs and the web data URIs consistent automatically.
- Conversion steps: run `python3 assets/images/love_pixel_art_gen.py` from the
  repository root. Re-running after a mask change updates device and web assets
  together.
- Destination: `assets/images/love_pixel_art.c` is compiled through
  `target_sources` in `main/CMakeLists.txt`; `tools/gen_admin_page.py` writes the
  web assets into `main/love_web_assets.h` (the raw background-tile PNG plus the page
  icon, served by `/bg.png` and by `/favicon.ico` and `/apple-touch-icon*.png`) and
  `main/love_admin_page.h` (HTML/CSS/JS, served as `/`, `/admin.css` and `/admin.js`).
- The web icons are **inlined into `admin.js`** as data URIs rather than served as
  `/icon/N.png`: all sixteen are only 2,641 bytes, and splitting them into separate
  requests makes a single page load open a dozen extra connections, squeezing the
  device heap until the Wi-Fi driver cannot allocate a transmit frame. The 167 KB
  pixel font, not the icons, was what made the page unloadable, and it is gone.
- License: the icons and tile are original artwork for this repository and use
  the repository license.
- Cost: about 111 KB of source and roughly 18 KB of Flash for icons, tile and
  palette (13.8 KB icons, 4.6 KB tile); I4 icons are converted row by row while
  drawing and are never cached as a full screen.

### Lunar calendar table (love_lunar_table)

- Generator: `tools/gen_lunar_table.py` (needs `pip install lunarcalendar`).
  It produces two files:
  - `main/love_lunar_table.h`: the device-side C table covering 2018–2050, one
    20-bit value per year (bit 0..3 leap month; bit 4..15 month lengths, 1 = 30
    days; bit 16 leap-month length).
  - `images/web/lunar.json`: the same table for the admin page, so both sides run
    identical arithmetic.
- **Why the web needs this table too**: the browser's
  `Intl.DateTimeFormat('zh-CN-u-ca-chinese')` disagreed with public sources in 2
  of 18 tested years (2027 off by +1 day, 2030 by −1). Using it for the preview
  produced "the page says 02-07, the device says 02-06". One shared table removes
  that class of disagreement.
- The generator self-checks: it recomputes every Spring Festival from the table it
  just built and compares against public-source dates hard-coded in the script
  (18 years, 2018–2035) plus 2026 Dragon Boat and Mid-Autumn. On any mismatch it
  exits non-zero and **writes no file**. A wrong lunar table shows wrong dates
  silently, which is worse than not having the feature, so this check is enforced.
- Outside the covered years the device returns failure and labels the UI as
  lunar-calendar-out-of-range instead of guessing a date.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.
