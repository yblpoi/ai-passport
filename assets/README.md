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

### love_font_12 / love_font_24 / love_font_36 (pixel fonts for the love countdown play)

The device UI uses a **bitmap pixel font** so it matches the pixel icons and the
pixel heart wallpaper. Only these three sizes are allowed.

- Files (all generated with `lv_font_conv`):
  - `fonts/love_font_12.c` — small body text: hints, start date, unit, battery,
    settings rows
  - `fonts/love_font_24.c` — main text: title, person names, event names
  - `fonts/love_font_36.c` — digits and `+-.:/%` only, used for the big day count
- Source and license: **Ark Pixel Font** 12 px size, Simplified Chinese (zh_cn),
  proportional; SIL Open Font License 1.1, shipped as `fonts/OFL-ark-pixel.txt`.
  The repository commits only the generated files and the conversion command,
  not the TTF.
- **Why only 12 / 24 / 36**: the font is hand-drawn on a fixed 12 px design grid.
  Only integer multiples (1×/2×/3×) keep every stroke on whole pixels. Measured
  `adv_w` and `box_w/h` scale exactly at 12/24/36/48; a non-integer factor makes
  stroke widths uneven and destroys the dot-matrix look. **Do not add a fourth
  size.**
- Character range: ASCII 0x20–0x7E, common CJK punctuation, and GB2312 level-1
  Han characters, intersected with the font's own cmap — 3718 characters land
  (the font lacks 175 level-1 characters, mostly rare ones). Missing glyphs fall
  back to Montserrat, so a rare character in a person's name may look wrong.
- Conversion command (repository root, `lv_font_conv` 1.5.3):

  ```bash
  lv_font_conv \
    --font <ark-pixel-12px-proportional-zh_cn.ttf> \
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
- Cost: about 330 KB of Flash altogether (1 bpp, uncompressed), read-only, not
  resident in internal RAM.
- Generate and register additional sizes separately instead of switching to a
  full CJK family to add a single character.

### ark12-subset.woff2 (the same pixel font for the admin page)

- File: `fonts/ark12-subset.woff2` (about 122 KB), same source and license.
- Purpose: the device preview on the admin page must show the same dot-matrix
  letterforms as the real device; otherwise only the coordinates line up and the
  pixel look is lost. `tools/gen_admin_page.py` and
  `tools/preview_admin_page.py` inline it as a base64 `data:` URI.
- Generation: subset the same TTF with `fonttools` to the same character set used
  on the device, then convert to woff2:

  ```bash
  pyftsubset ark-pixel-12px-proportional-zh_cn.ttf \
    --text-file=<charset file> --flavor=woff2 --no-hinting --desubroutinize \
    --layout-features='' --output-file=assets/fonts/ark12-subset.woff2
  ```

- Cost: about 163 KB added to the page after base64 encoding, which also lives in
  firmware Flash.

## Images

Store reusable source images and generated display assets in `images/`.

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

### Love countdown pixel art (love_pixel_art)

- Source: `images/love_pixel_art_gen.py` (8×8 pixel masks plus a palette, no
  third-party dependencies).
- Generated output:
  - `images/love_pixel_art.c` and `main/love_pixel_art.h`: sixteen 40×40
    ARGB8888 icons (masks scaled 5× — only integer factors avoid half pixels), a
    48×48 RGB565 heart background tile, and `love_pixel_palette[16]`.
  - `images/web/`: PNGs exported from the same masks, `icons.json`/`assets.json`
    data URIs, the palette (`palette` field), and `contact-sheet.png` for manual
    review, all used by the admin page.
- The palette order is the 4 bpp index order for uploaded custom avatars
  (`PALETTE_ORDER`). **Changing it recolors every avatar already uploaded**, so it
  is written out explicitly rather than relying on dict order.
- Why there is no circular avatar chip: a circle necessarily clips the four
  corners of a square icon, and 15 of the 16 characters lost solid pixels in
  testing (the cat lost 186, the gift 284 — ears and corners got flattened).
  Keeping the full silhouette matters more.
- Conversion steps: run `python3 assets/images/love_pixel_art_gen.py` from the
  repository root. Re-running after a mask change updates device and web assets
  together.
- Destination: `assets/images/love_pixel_art.c` is compiled through
  `target_sources` in `main/CMakeLists.txt`; the web assets are inlined into
  `main/love_admin_page.h` by `tools/gen_admin_page.py`.
- License: the icons and tile are original artwork for this repository and use
  the repository license.
- Cost: about 326 KB of source and roughly 100 KB of Flash for icons, tile and
  palette; ARGB8888 icons are converted while drawing and are never cached as a
  full screen.

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
