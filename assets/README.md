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

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Hardware overview infographic embedded in both `docs/README.md` files. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

### Commemorative-day pixel art (love_pixel_art)

- Source: `images/emoji/` — sixteen Twemoji graphics, vendored as 72×72 PNGs with
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
  configuration, so reordering would silently change their icons. The historical
  slot names are kept even where the art changed: slot 9 is still `LOVE_ICON_MOON`
  and now holds a moon cake, slot 14 is still `LOVE_ICON_LEAF` and now holds a
  smiling face with hearts.
- Generated output:
  - `images/love_pixel_art.c` and `main/love_pixel_art.h`: sixteen 40×40 4 bpp
    indexed (I4) icons (the 20×20 grid scaled 2×), a 48×48 RGB565 heart background
    tile, and `love_pixel_palette[16]`.
  - Each icon carries **its own** 16-colour palette: the union of the sixteen
    graphics' colours is far past sixteen entries, and the `lv_bin_decoder`
    convention already puts the palette at the head of each image's data. An icon
    that ever needs more than 15 opaque colours fails the generator loudly in
    `per_icon_palette()` rather than losing colours silently; the busiest of the
    sixteen (the tree) uses 8.
  - The icons are I4 rather than ARGB8888: a 16-colour palette is embedded at the
    head of each icon's data (as `lv_color32_t`, memory order B,G,R,A), followed by
    the indices, two pixels per byte, high nibble first. That is the
    `lv_bin_decoder` convention for `LV_IMAGE_SRC_VARIABLE` plus an indexed format,
    so `LV_BIN_DECODER_RAM_LOAD` is not needed and rows are converted on demand
    while drawing. Index 0 of the per-icon palette is fixed to transparent (the
    icons are hollow), which is a separate table from the avatar palette below.
  - `images/web/`: PNGs exported from the same grids, `icons.json`/`assets.json`
    data URIs, the palette (`palette` field), and `contact-sheet.png` plus
    `contact-sheet-zoom.png` (3× nearest-neighbour on a checkerboard) for manual
    review, all used by the admin page.
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
  background, not square photos. Measured on the sixteen generated grids, only the
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
  `/icon/N.png`: all sixteen are 3,949 bytes as PNGs (5,644 bytes as data URIs),
  and splitting them into separate requests makes a single page load open a dozen
  extra connections, squeezing the device heap until the Wi-Fi driver cannot
  allocate a transmit frame. The 167 KB pixel font, not the icons, was what made
  the page unloadable, and it is gone.
- License: the icons are **Twemoji graphics** (https://github.com/jdecked/twemoji,
  tag v16.0.0), pixelated as described above and therefore derivative works that
  remain under **CC-BY 4.0** — see `images/emoji/LICENSE-GRAPHICS.txt` for the full
  text, which must stay with the art. The heart background tile is original artwork
  for this repository and uses the repository license.
- Cost: about 112 KB of source and 18 KB of Flash for icons, tile and palette
  (13.8 KB icons — sixteen 864-byte I4 images — plus a 4.6 KB tile); I4 icons are
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
