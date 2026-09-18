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
- Character range: ASCII 0x20–0x7E, common CJK punctuation, the complete GB2312
  level-1 Han set (3755), the GB2312 level-2 set (3008), and ten given-name
  characters that GB2312 does not have at all (U+73A5, U+5586, U+6607, U+9814,
  U+73FA, U+5A73, U+71DA, U+579A, U+7287, U+752F) — **6910 requested**. The source
  font has no glyph for 126 of them (obscure level-2 characters only); the
  generator skips those silently. The charset is rebuilt with Python's `gb2312`
  codec rather than a hand-kept list: level 1 is the range with a lead byte of
  0xB0..0xD7, level 2 is 0xD8..0xF7, and both are enumerated over trail bytes
  0xA1..0xFE.
- **Why level 2 is included:** level 1 does not contain the given-name characters
  that show up in real names — the ones for "graceful", "prosperous", "joyful" and
  "pretty" are level 2 (U+5A77 = 0xE6C3, U+946B = 0xF6CE), and a few, such as
  U+73A5 and U+6607, are not in GB2312 at all. With only level 1 in the font,
  LVGL fell back to `LV_USE_FONT_PLACEHOLDER` and drew a full-line-height solid
  block where the character should be.
- An earlier build took the font straight from Ark Pixel Font, whose cmap is
  missing 172 level-1 characters, including everyday ones such as the characters
  for "hot", "execute", "love", "fate", "however", "window", "tight", "police" and
  "medicine". Those had no glyph anywhere, so LVGL drew placeholder boxes.
  Regenerating from Fusion Pixel Font adds exactly those 172 glyphs (they are still
  present).
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
- Cost: about 634 KB of Flash altogether (12 px 170 KB, 24 px 462 KB, 36 px 1 KB,
  measured from the map file; 1 bpp, uncompressed), read-only, not resident in
  internal RAM. Extending the charset from level 1 to level 2 grew both files by
  about 271 KB in the application image; the 4 MB factory partition leaves ~74 %
  free after that.
- Generate and register additional sizes separately instead of switching to a
  full CJK family to add a single character.
- Note: the generated files carry an `Opts:` header comment holding the absolute
  paths used at generation time (`lv_font_conv` writes `--font` and `--output`
  verbatim, which is normal). On a different machine, or after the checkout moves,
  that path will not match the repository location. It is provenance metadata only
  and does not affect glyph data, which depends solely on the font file, symbol
  set, size and bpp. Re-run the command above to refresh it; do not hand-edit
  generated files. The symbol set can be copied verbatim out of the existing
  `Opts:` line.

### Why the admin preview has no pixel font

- The preview is laid out on the device's 240×320 logical pixels - coordinates, font
  sizes and line heights are exact - but its glyphs come from the browser's system
  fonts. There is no `@font-face` on the page.
- A subset of Ark Pixel 12px used to be embedded (3,891 glyphs: 95 ASCII, about 40
  CJK punctuation marks and the 3,755 GB2312 level-1 ideographs; about 122 KB as
  woff2) so that the preview showed the device's dot-matrix letterforms too. It was
  dropped for size: the page's own fixed text uses only 475 Chinese characters, and
  covering *any* typed name meant shipping twenty times the rest of the page.
  Serving it as its own route with an hour of caching was still too heavy.
- The file has been deleted from the repository (it was
  `fonts/ark12-subset.woff2`). To bring the idea back, get the source TTF as
  described in the section above and repack it:

  ```bash
  pyftsubset ark-pixel-12px-proportional-zh_cn.ttf \
    --text-file=<charset file> --flavor=woff2 --no-hinting --desubroutinize \
    --layout-features='' --output-file=assets/fonts/ark12-subset.woff2
  ```

  Re-enabling it also means re-adding the font input in `tools/gen_admin_page.py`,
  the `/font.woff2` route in `main/love_httpd.c` and the `@font-face` rule in
  `assets/web/admin.css`. Note the charset is no longer the 3,755 characters of its
  day: the device fonts now cover level 1 plus level 2 (6,910 characters), so
  rebuilding from the `Opts:` line of `love_font_12.c` would give roughly twice the
  bytes.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

### Commemorative-day pixel art (love_pixel_art)

- Source: `images/emoji/` — eighteen Twemoji graphics, vendored as 72×72 PNGs with
  the CC-BY 4.0 graphics license text (`LICENSE-GRAPHICS.txt`) and a `manifest.json`
  that pins the upstream tag (v16.0.0), the file names, the code points and a sha256
  per file. `images/fetch_emoji.py` fetches them (fixed tag, sha256 verified, exits
  non-zero on any mismatch); it is a maintenance script and takes no part in the
  build.
- From art to icon: `images/love_pixel_art_gen.py` (no third-party dependencies)
  trims each graphic's transparent margin, scales it **proportionally** to a 20×20
  logical grid, centres it there, and then scales that grid 2× to the 40 px the
  display shows — two screen pixels per logical pixel, which is what keeps the
  pixel structure visible on the 240×320 panel. Every output cell takes the
  alpha-weighted vote of the source pixels it covers — the dominant colour, not an
  average, so the result stays hard-edged pixel art instead of a blurry
  low-resolution vector. Proportional scaling is not a preference: the trimmed
  boxes run from 38×72 (balloon) to 72×72 (cake), so filling both axes would
  stretch the balloon almost 2×.
- This replaced the hand-drawn masks of 2026-09-18. Those could not be produced
  reliably — the design sheets came from an AI image generator whose "pixels" do
  not land on a real grid, so its output cannot be downsampled into this format,
  and every revision had to be laid out by hand on the 8×8 grid. The pixel-art
  style now comes from the source art's own shapes, sampled onto a real grid.
- The icon **order must not change**: `LOVE_ICON_*` indices are stored in the user's
  configuration, so reordering would silently change their icons — and adding icons
  changes the *numbering* every stored configuration was written with.
  Slot 9 keeps the name `LOVE_ICON_MOON` even though the art is now a moon cake - a
  moon cake is still a moon cake. Slot 14 was renamed from `LOVE_ICON_LEAF` to
  `LOVE_ICON_LOVING`: it holds a smiling face with hearts, and a name about leaves
  would only mislead whoever reads the code next. Both are just names - the stored
  configuration holds the slot *index*, so nothing on a user's device changed.
- **New icons may only be appended.** The set grew from 16 to 18 on 2026-09-18
  (firecracker 16, bouquet 17) so the calendar defaults could use them. The four
  custom-avatar slots therefore moved from 16..19 to 18..21, so configuration
  version 5 shifts any icon number >= 16 by two when reading v4 and older records
  (`icon_v4_to_v5()` in `main/love_config.c`, covered by a host test).
- Generated output:
  - `images/love_pixel_art.c` and `main/love_pixel_art.h`: eighteen 40×40 4 bpp
    indexed (I4) icons (the 20×20 grid scaled 2×), a 48×48 A8 heart background
    tile, and `love_pixel_palette[16]`.
  - The tile **draws hearts only, never a background**, and it carries only the
    opacity of the pattern, so it is stored as **A8** (one alpha byte per pixel,
    **2,304 bytes**); the colour comes from `bg_image_recolor` (white, opaque) in
    `main/love_app.c`, which draws a white heart at the tile's own 30% alpha.
    The old RGB565 version was 4,608 bytes. **Why not the smaller I1**: tried,
    and a full screen went from 104 ms to **396 ms** (45-56 ms per band). With
    `LV_BIN_DECODER_RAM_LOAD` off, an indexed image is decoded row by row into
    ARGB8888 and blended, so tiling re-decodes hundreds of rows per band; A8 takes
    the `cf == LV_COLOR_FORMAT_A8` path in `lv_draw_sw_img.c` that treats the
    whole image as a mask and fills with the recolor colour, reading one byte per
    pixel. Measured on one board, 240x40 buffer, 8 bands: RGB565 104 ms,
    A8 103 ms, I1 396 ms.
    The background colour still comes at runtime from the **screen style's
    `bg_color`** (on the web side, `background-color` on `body` and `.screen`; see
    `render()` in `main/love_app.c` and `--pink` in `assets/web/admin.css`), so
    changing it is a one-value edit that needs neither regenerating the art nor
    reflashing the tile. Avatars work the same way: the asset carries only shape
    and colour, while the parts that vary — background, corner radius — live at
    runtime.
  - Each icon carries **its own** 16-colour palette: the union of the eighteen
    graphics' colours is far past sixteen entries, and the `lv_bin_decoder`
    convention already puts the palette at the head of each image's data. An icon
    that ever needs more than 15 opaque colours fails the generator loudly in
    `per_icon_palette()` rather than losing colours silently; the busiest of the
    eighteen (the moon cake) uses 10.
  - The icons are I4 rather than ARGB8888: a 16-colour palette is embedded at the
    head of each icon's data (as `lv_color32_t`, memory order B,G,R,A), followed by
    the indices, two pixels per byte, high nibble first. That is the
    `lv_bin_decoder` convention for `LV_IMAGE_SRC_VARIABLE` plus an indexed format,
    so `LV_BIN_DECODER_RAM_LOAD` is not needed and rows are converted on demand
    while drawing. Index 0 of the per-icon palette is fixed to transparent (the
    icons are shapes on a transparent background), which is a separate table from
    the avatar palette below.
  - `images/web/`: PNGs exported from the same grids, `assets.json` (the tile and
    icon data URIs plus the `palette` field - the only file the admin page inlines,
    which is why there is no second copy of the icon list), and
    `contact-sheet.png` plus `contact-sheet-zoom.png` (3× nearest-neighbour on a
    checkerboard) for manual review.
- The palette order is the 4 bpp index order for uploaded custom avatars
  (`PALETTE_ORDER`). **Changing it recolors every avatar already uploaded**, so it
  is written out explicitly rather than relying on dict order.
- Avatar corner rounding: radius **4 px**, applied **only to uploaded custom avatars**
  (photos, which are full-bleed squares). The built-in icons are deliberately left
  alone. The test lives in one place, `ui_pixel_corner_cut()` in
  `main/ui_pixel_math.c`, and a host test pins the four corners as **exact mirrors**
  of each other -- an earlier version used a negative value as a "not in a corner"
  sentinel, and only the bottom-right corner ended up rounded.
  A **circular** chip is still rejected for the same reason: the built-in icons are
  shapes, not tiles, and a circle would clip whichever of them reaches the edge.
  **Why the built-in icons are not rounded:** they are *shapes* on a transparent
  background, not square photos. Measured on the eighteen generated grids, only the
  cat's two top corners touch the canvas edge at all; a 4 px radius would nick
  those and leave the rest untouched, so the rule would apply inconsistently to
  exactly the one icon that needs its corners. For a shape the choice is only
  "leave it alone" or "nick it", so it is left alone — and the rounding rule stays
  a property of *custom avatars*, which are full-bleed photos.
  Both sides agree: `assets/web/admin.css` rounds only custom-avatar images
  (`img.rounded`), and the device points custom-avatar corners at a transparent
  palette index while packing the image as I4, using the same
  `ui_pixel_corner_cut()` mask (the avatar palette has no transparent entry of its
  own, so one unused index is given alpha 0).
- Avatar photos are **processed in the browser**; the device only ever receives
  40×40, 800 bytes of 4 bpp index data (`POST /api/avatar`). The web side runs
  `assets/web/avatar_slic.js`: centre-crop to a square, scale to a 240×240 working
  image, cluster it into regions with **SLIC superpixels** (Lab colour plus
  position), then let each 40×40 output cell take a majority vote and paint it with
  that region's mean colour before mapping to the device's sixteen. Nearest-colour
  per pixel turns a photo's gradients into a dot screen; merging by region first
  makes the colour blocks follow the boundaries of a face, hair or background. The
  three strengths (soft/standard/strong) set the superpixel granularity and the
  iteration count. The kernel lives in its own file so a host test can run it
  directly: `tests/test_avatar_slic.mjs` loads it with node's `vm` and asserts on
  synthetic images, while `tools/gen_admin_page.py` inlines it into `admin.js` — a
  page load still makes one `/admin.js` request.
- **How much detail survives is decided by `step`, not by `mode`**: the larger the
  grid spacing S is relative to the 6 px output cell, the more cells share one
  region and therefore one colour — soft(8) is about 1.3 cells per region,
  normal(14) about 2.3, strong(22) about 3.6. With only 1600 cells in a 40x40
  output, normal leaves an effective spatial resolution of roughly 17x17. The
  default is soft for that reason. This only became clear after the user reported
  "no detail left at all", which had first been blamed on the iteration count.
- `mode` decides **which colour that region gets** (four options):
  **cell** averages the region's pixels *inside this cell* (the default: neighbouring
  cells of one region follow the local shading, so detail comes back without the
  speckle of per-pixel sampling); **center** samples the single original pixel at the
  region's centre (what the reference implementation does — more contrast, but every
  cell of a region still gets the same colour); **vote** takes the most common palette
  colour inside the region; **mean** averages the whole region (the flattest, and the
  only option at first — the one the user called "no detail at all").
- The web page exposes step/iters/weight/mode in an advanced panel with a **live
  preview** (the 40x40 result at 4x, drawn with the device's own sixteen colours,
  recomputed within ~100 ms). The three strength presets are just combinations of
  those numbers; touching a slider switches the preset to "custom". The preview
  source is the photo just picked, or the existing custom avatar from the device
  when no new photo has been chosen.
- Conversion steps: run `python3 assets/images/love_pixel_art_gen.py` from the
  repository root. It reads `images/emoji/*.png`, so swapping an icon means editing
  the `EMOJI` list in the generator (name, label, code point); changing the art
  itself also means updating `images/fetch_emoji.py` and re-vendoring. Then re-run
  `python3 tools/gen_admin_page.py` to fold the new web assets into
  `main/love_admin_page.h` and `main/love_web_assets.h`.
- Destination: `assets/images/love_pixel_art.c` is compiled through
  `target_sources` in `main/CMakeLists.txt`; `tools/gen_admin_page.py` writes the
  web assets into `main/love_web_assets.h` (the raw background-tile PNG plus the page
  icon, served by `/bg.png` and by `/favicon.ico` and `/apple-touch-icon*.png`) and
  `main/love_admin_page.h` (HTML/CSS/JS, served as `/`, `/admin.css` and `/admin.js`).
- The web icons are **inlined into `admin.js`** as data URIs rather than served as
  `/icon/N.png`: all eighteen are 4,519 bytes as PNGs (6,452 bytes as data URIs),
  and splitting them into separate requests makes a single page load open a dozen
  extra connections, squeezing the device heap until the Wi-Fi driver cannot
  allocate a transmit frame. The 167 KB pixel font, not the icons, was what made
  the page unloadable, and it is gone.
- License: the icons are **Twemoji graphics** (https://github.com/jdecked/twemoji,
  tag v16.0.0), pixelated as described above and therefore derivative works that
  remain under **CC-BY 4.0** — see `images/emoji/LICENSE-GRAPHICS.txt` for the full
  text, which must stay with the art. The heart background tile is original artwork
  for this repository and uses the repository license.
- Cost: about 119 KB of source and 18 KB of Flash for icons, tile and palette
  (15.2 KB icons — eighteen 864-byte I4 images — plus a 2,304-byte tile); I4 icons are
  converted row by row while drawing and are never cached as a full screen.

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
