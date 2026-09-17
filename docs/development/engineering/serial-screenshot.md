<p align="right">
  <a href="serial-screenshot.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Capturing the device screen over the serial console

Layout work on this device used to need a person looking at the 240x320 panel.
`shot` (and its protocol alias `FAP_SCREENSHOT_V1`) dumps the current screen over
the USB serial console instead, and `key` injects a button press so every screen
can be reached without touching the device.

## Protocol

The host sends one ASCII line; the device answers with a header line followed by
exactly that many bytes of little-endian RGB565 pixels:

```text
> FAP_SCREENSHOT_V1
FAP_SCREENSHOT_V1 240 320 RGB565LE 153600
<153600 bytes>
```

`tools/screenshot.py` speaks it and writes a PNG:

```bash
python3 tools/screenshot.py                 # 240x320 -> /tmp/screen.png
python3 tools/screenshot.py -o list.png     # choose the output
python3 tools/screenshot.py --raw dump.bin  # also keep the raw pixels
```

It needs no third-party library beyond `pyserial` (the PNG is written with
`zlib`). The protocol itself is the one described in
`docs/reference/y2lin/serial-screenshot-protocol.md`; that document is another
application's write-up of the same protocol and is worth reading for the
pitfalls, but its central technique does not apply here:

## Why this implementation does not reserve a framebuffer

That reference reserves a full 240x320x2 = 150 KB static buffer and renders into
it with `lv_snapshot_take_to_draw_buf()`. It ran on a firmware with roughly
220 KB of free heap; this one has about 204 KB of RAM in total and 27 KB free
with Bluetooth on, so a static 150 KB buffer would simply not boot.

Instead `main/love_shot.c` hooks `LV_EVENT_FLUSH_START` — the same event
`bsp_display_lvgl.c` already uses for its corner mask — and streams each flush
chunk straight out as it arrives. The panel's LVGL draw buffer is 240x20, so a
full-screen refresh arrives as 16 full-width bands that concatenate into exactly
the image the protocol promises. That assumption is checked per chunk: if a chunk
is not full width or does not start where the previous one ended, the transfer is
abandoned rather than sending a torn image.

## Things that will bite you

- **Render on the LVGL task, not the console task.** A full-screen software
  render needs about 7 KB of stack (the LVGL port's own task uses 7168), while the
  console task has 4096. Rendering there panics with a stack protection fault. The
  command therefore posts the capture with `lv_async_call()` and waits on a
  semaphore.
- **Silence every log for the duration.** The host reads exactly the declared
  byte count, so one log byte inside the window corrupts the image. The command
  sets `esp_log_level_set("*", ESP_LOG_NONE)` before the first header byte and
  restores the previous level afterwards.
- **Write in chunks smaller than the transmit ring buffer.** The console REPL
  installs the USB-Serial-JTAG driver with its default 256-byte buffers, and
  `usb_serial_jtag_write_bytes()` fails immediately for anything larger. Chunks
  are 128 bytes.
- **The driver has to be installed.** Reading that port without it dereferences a
  null driver object and the board reboots in a loop (the screen appears to
  flash). `love_console_start()` installs it; the capture refuses to run if it is
  missing.
- **A whole `love_config_t` does not fit on these stacks any more.** From config
  v4 it is 1454 bytes (24 events), which overflowed the console and HTTP task
  stacks when kept as a local. Those paths allocate it from the heap instead. The
  `status` command prints the remaining stack of the console, HTTP, LVGL and
  NimBLE tasks, which is how that was found and how a future field can be checked.

## Driving the UI from the console

`key up|down|ok|long` runs one button event through `love_app_key()` exactly like
a real press. Together with `shot` this makes every screen verifiable from a
script:

```bash
python3 tools/screenshot.py --keys down,down,down -o /tmp/shots
```

`--keys` walks the pages in **one serial session** and writes `00-start.png`,
`01-down.png`, … .

**It has to be one session.** Opening the port resets the chip, and which page the
device is on lives in RAM — running `tools/screenshot.py` once per screen resets
you back to the home screen every time, so the later shots silently show the wrong
page. Use `--keys`, or keep a single `serial.Serial` open yourself.

Paging on this firmware: `down` from the home screen walks the single-page event
cards first, then the list pages, then back to the home screen; `up` goes the other
way. The page number on screen counts the whole carousel — single-page cards and
list pages share one numbering. `status` reports the current screen and page on one
line (the device prints that line in Chinese, see the linked Chinese document),
which checks a whole walk without taking any screenshots at all.

Notes: the device must be on USB (the pixels go over the serial console, and
Bluetooth notifications would be far too slow); opening the port replays whatever
the driver still holds from boot, so read past that before trusting the output;
and `key` renders on the console task, which leaves it with little headroom —
the real buttons render on the esp_timer task instead (3584 bytes). A key injected
while the screen is blanked only wakes the screen (that is the product rule), so
turn on `debug on` before walking pages.