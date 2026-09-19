<p align="right">
  <a href="avatar-pixelation.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Avatar Pixelation

Uploading a photo in the admin page turns it into the 40×40, 16-colour, 4 bpp avatar the
device stores and draws. Every step of the image processing happens in the browser, in
the page's own JavaScript; the device never decodes a photo.

This is the authoritative description of that algorithm and of the contracts around it.
[`assets/README.md`](../../../assets/README.md) covers where the art, fonts, and icons
come from; this document covers how a photo becomes an avatar.

## Where it sits

| Layer | File | Responsibility |
| --- | --- | --- |
| Kernel | `assets/web/avatar_pixel.js` | Pure functions: histogram, palette, per-cell colour, cleanup, packing. No DOM. |
| Page | `assets/web/admin.js` | Decode, centre-crop, call the kernel, live preview, upload. |
| Build | `tools/gen_admin_page.py` | Inlines the kernel into `admin.js` and compiles both into `main/love_admin_page.h`. |
| Device | `main/love_httpd.c`, `main/love_store.c` | Accept 864 or 800 bytes, store indices and palette, pack them for LVGL. |

The kernel is a separate file so that `tests/test_avatar_pixel.mjs` can load **the code
that ships** through node's `vm` and assert on it. The generator inlines that same file,
so there is exactly one implementation, not a test double.

## The pipeline

The input is a 240×240 RGBA working image: the browser centre-crops the photo to a square
and scales it to 240×240, so each of the 40 output cells covers one 6×6 source block.

1. **Features.** A 5-bit-per-channel RGB histogram of the opaque pixels (32768 buckets),
   then CIELAB for every bucket that actually occurs. Each bucket is used three times
   later, so this pass is paid once; it dominates the kernel's cost.
2. **Palette.** Median cut to the requested number of colours, then three Lloyd (k-means)
   iterations in Lab, then a final pass that replaces each cluster centre with the mean of
   the **real** pixels in that cluster. That last pass matters: 5-bit bucket centres top
   out at 247, so pure white would otherwise shift a step down. Fewer than 16 colours are
   padded, because the device's I4 image always carries a 16-entry palette.
3. **Per-cell colour.** Every pixel votes for the palette entry nearest to it in Lab,
   looked up through a table over the 5-bit buckets. A strict majority wins. A cell with
   no majority falls back to the entry nearest the cell's mean. Both averages are weighted
   by the sampling kernel described below.
4. **Cleanup.** Optional and off by default; see *Cleanup* below.
5. **Packing.** 16 little-endian `0x00RRGGBB` words, then two pixels per byte with the high
   nibble first.

### Why voting per cell instead of nearest colour per pixel

Nearest colour per pixel turns a photo's gradients into a dot screen. Merging inside the
cell first makes each colour follow the boundaries of a face, hair, or background — the
difference between pixel art and a mosaic.

### Why a majority rule with a mean fallback

A cell that is genuinely mixed — a skin gradient, the edge of a strand of hair — gives two
similar colours roughly equal votes. Taking the plurality there would make neighbouring
cells alternate between them and read as noise, so the fallback is the colour nearest the
cell's mean. The majority rule is what keeps small dark features: a cell that is 60% dark
is dark, even though its mean is mid-tone.

### Cleanup

Sixteen colours cannot describe a continuous gradient without banding, so some cell will
land on a colour that differs from all four orthogonal neighbours. At 40 px that reads as
dirt: on the sample photos, 92 and 80 of the 1444 interior cells were isolated before this
step existed. `cleanIsolated()` replaces such a cell with its most common neighbouring
colour, but only when all three of these hold:

1. the cell shares its colour with none of its four orthogonal neighbours;
2. its own colour appears at most `AVA_CLEAN_MAX_OWN` (2) times in the 3x3 window — a
   diagonal run has no orthogonal neighbour of its own colour, so without this guard a
   three-cell diagonal stroke would be eaten as noise;
3. some other colour holds at least `AVA_CLEAN_MIN` (3) of the nine cells.

The threshold of 2 is measured rather than guessed: at 1, most of the speckle survives
(92 to 45 cells); with no guard at all, diagonals of three cells and up are eaten too
(92 to 5). Two passes run, because the first exposes cells the second can then fix. There
are no random decisions anywhere in it, so the result is reproducible.

Cleanup is off by default; the `cleanup` rows of the table below are what it costs.


## The five knobs

The panel exposes all five; each changes the picture in a way you can see.

| Knob | Values | Default | What it changes |
| --- | --- | --- | --- |
| Colour count | 4–16 | 16 | How many colours the photo's own palette gets. Fewer means larger flat areas and a more obviously stylised look. |
| Palette | per-photo, device | per-photo | The device's sixteen were chosen for the icons and have almost no mid-tones. Switching to them is the largest single change available. |
| Sampling | area, center, point | area | How much detail each cell keeps. `area` averages the 36 source pixels; `point` takes one pixel at the cell's centre and is the sharpest and the noisiest. |
| Assignment | vote, mean | vote | Whether small dark areas survive inside a cell. `vote` keeps a pupil dark; `mean` smooths it away. |
| Cleanup | 0–3 passes | 0 | Removes cells that differ from all four neighbours. Cleaner on a 40 px screen, at the cost of some real detail. |

## Measured behaviour

Two sample photos, both centre-cropped to 240×240: a dog portrait and a bird photo. Each
row changes one knob and leaves the rest at their defaults. The `ms` column is one timed run
per setting and swings with warm-up, so read it as an order of magnitude rather than a
benchmark.

| Setting | dog ΔE | dog colours | dog isolated | bird ΔE | bird colours | bird isolated | ms (dog/bird) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| default | 5.4 | 16 | 92 | 6.7 | 16 | 80 | 24 / 27 |
| colour count 12 | 6.5 | 12 | 65 | 7.6 | 12 | 69 | 10 / 13 |
| colour count 8 | 7.4 | 8 | 48 | 9.4 | 8 | 43 | 6 / 9 |
| palette: device | 18.7 | 7 | 22 | 17.5 | 10 | 22 | 2 / 3 |
| sampling: center | 5.6 | 16 | 110 | 6.8 | 16 | 102 | 15 / 19 |
| sampling: point | 5.9 | 16 | 133 | 7.2 | 16 | 134 | 9 / 15 |
| assignment: mean | 5.3 | 16 | 100 | 6.7 | 16 | 78 | 16 / 23 |
| cleanup 1 | 6.0 | 16 | 20 | 7.0 | 16 | 21 | 9 / 16 |
| cleanup 2 | 6.1 | 16 | 12 | 7.1 | 16 | 10 | 9 / 15 |
| cleanup 3 | 6.1 | 16 | 11 | 7.1 | 16 | 7 | 9 / 16 |

The columns mean:

- **ΔE** — mean CIELAB distance (D65) between the colour a cell ends up showing and the
  mean colour of the 6×6 source block it covers. Lower is closer to the photo.
- **colours** — distinct palette entries the output actually uses, out of 16.
- **isolated** — cells whose colour differs from all four orthogonal neighbours, out of the
  1444 interior cells. This is the speckle metric: at 40 px it reads as dirt.
- **ms** — kernel time for one 240×240 to 40×40 conversion, node 22 on Apple silicon.

Two things this table settles. First, **colour is what decides whether the result looks
right**: the downsampling kernel is not where the fidelity lives, and switching to the
device's built-in palette costs three times more accuracy than any other knob on the list.
Second, **cleanup is a trade, not a fix**: it removes 87% of the isolated cells and pays
for them in ΔE (5.4 to 6.1 on the dog, 6.7 to 7.1 on the bird). That is why it is a knob
with 0 as its default rather than a step in the pipeline.

## How the design got here

The first implementation clustered the working image into SLIC superpixels, gave each
region one colour, and quantised the result to the device's fixed sixteen. Measured on the
same two photos, that combination produced ΔE 18.6 / 17.7 and rendered a dark background as
`#17202A` navy — within about 0.2 of what the `palette: device` row still measures today,
which is the point: the palette, not the kernel, was the problem. Two causes, in order of
size:

1. The device's sixteen were chosen for the icons. They are cartoon colours with almost no
   mid-tones, so shadows and skin land on whatever loud colour happens to be nearest.
2. At a 40×40 output a superpixel region is barely larger than a single cell, so the
   per-region vote degenerated and the boundary between two regions showed up as a block.

Both are gone: the palette is now taken from the photo, and colour is decided per output
cell.

The reference implementation for this kind of tool — `pixeltool.art`'s pixel worker — does
something else again, and it is worth being explicit about which parts were taken:

- **Kept:** a median cut over the image's colours to build the palette, and a cached
  5-bit-per-channel lookup table so mapping a pixel to a palette colour is a table read.
- **Not kept:** point sampling. That tool disables smoothing and samples one source pixel
  per output pixel, which is correct at its 64–256 pixel output width and wrong at 40:
  taking one pixel out of every 6×6 block turns a photo into snow. Averaging the block is
  what this kernel does instead, which is exactly why the per-cell vote above is needed.
- **Not kept:** ordered dithering. At 40×40 it turns the whole image into a dot screen.

## Contracts a change must keep

- **Pure and deterministic.** No randomness anywhere in the kernel; the same input must
  produce the same indices and the same palette. A host test pins this.
- **A classic script, not a module.** No `import` or `export`, and no name that
  `admin.js` already uses (`PALETTE`, `AVA`, `AVATAR_BYTES`, and so on) — the file is
  inlined into the page, not loaded beside it.
- **The wire format is 864 bytes**: 64 bytes of palette followed by 800 bytes of indices.
  A body of exactly 800 bytes is also accepted and means "use the built-in sixteen" — that
  is the path an older page or a hand-written request takes.
- **Layouts.** Palette: 16 entries, each a little-endian `0x00RRGGBB`. Indices: 4 bpp, two
  pixels per byte, high nibble first.
- **Per-avatar palettes live under their own NVS keys** (`ap0`..`ap3`, 64 bytes each)
  rather than inside the avatar record, so a record written by an older firmware still
  reads back and simply has no palette. The missing key *is* the fallback to the built-in
  sixteen; no migration step exists or is needed.
- **Corner rounding is 4 px and applies only to custom avatars.** Built-in icons are
  shapes on a transparent background and are deliberately left square; the reasoning is in
  [`assets/README.md`](../../../assets/README.md).

## Changing it

```bash
node tests/test_avatar_pixel.mjs        # kernel behaviour, and "the shipped script parses"
python3 tools/gen_admin_page.py         # mandatory after any assets/web change
./tools/validate.sh --static            # repository checks and host tests
./tools/validate.sh                     # adds the ESP-IDF build and merged-image check
```

The generator step is not optional: `main/love_admin_page.h` is compiled into the
firmware, so a change under `assets/web/` that is not regenerated never reaches the
device, and the running page and the repository silently disagree.

## Budget

- **Kernel cost**: single-digit to about 30 ms per conversion on the sample photos
  (`node` 22, Apple silicon), depending on the settings and on whether the run is warm. The
  page's own live preview measured 3–15 ms in Chromium for the same work, which is what the
  "ms" readout under the preview shows.
- **Served size**: `/admin.js` is 87791 bytes, against 66452 before this work. The device
  serves it as a single response; measured on hardware it arrives in 0.36 s. Headroom is
  finite — a 214923-byte response previously exceeded the socket send timeout over the
  device's own access point, and that is why the page is split into `/`, `/admin.css` and
  `/admin.js` rather than served as one document.

## Known limits

- `palette: device` ignores the colour count; the built-in sixteen are fixed.
- There is no dithering, by choice.
- 40×40 is the real constraint on detail: a pupil or a nostril is one cell, so some
  features cannot survive regardless of the knobs.
- The kernel's host test runs on synthetic images. The quality figures above come from the
  two sample photos, not from the test.
