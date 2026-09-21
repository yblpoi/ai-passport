<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Pixel Anniversary Ornament — the days you share, on your desk

This branch (`feature/love-anniversary-pixel`) turns the FoloToy AI Passport board into one
finished application: a desk ornament that counts the days since a couple's start date and
down to the dates they care about. There is no account, no cloud service, and no companion
app — everything runs on the device, and the device serves the page used to configure it.

Upstream's platform overview stays at [docs/README.md](docs/README.md). This file documents
the firmware on this branch only.

The device boots straight into the application. There is no test menu and no intermediate
screen: everything is one home screen, one carousel of event pages, one settings page, and
one local status page.

## What the device shows

| Screen | Contents |
| --- | --- |
| Home | Two people with their icons or photos, the label for "days together", the big number, the start date, and the battery level in the top-right corner |
| Event card | One event per screen: icon, name, big countdown, and the target date the countdown folded to |
| List page | Four events per page, grouped by category, each row showing its own countdown. A list page has no cursor; up/down only turn the page |
| Settings | Background hotspot on/off, the address to open the admin page, the hotspot password, the network state, the time state, the auto blank-off step, the Bluetooth serial switch, and the local status page |
| Status | Battery percentage and voltage, network, time and its source, Bluetooth state, free memory, uptime, and per-task stack headroom |
| Confirm | One question with "allow" (short press) and "refuse" (long press). It appears when a command arrives over Bluetooth |

The home screen and the pages form one closed ring: home → single-page event cards → list
pages → home. The page number counts the whole ring, so cards and list pages share one
numbering. Up/down past either end of the ring returns to home.

## Controls

Three buttons: up, down, and ok. One physical press produces several events from the button
driver; only the judged event (short click, double click, or long press) counts as an action.

| Gesture | Where | Result |
| --- | --- | --- |
| up / down (short) | home | Enter the carousel: down starts at the first page, up starts at the last one |
| up / down (short) | carousel | Previous or next page |
| up (double press) | any carousel page | Jump straight back to home |
| ok (short) | carousel | Nothing; a list page has no cursor, and dates are edited in the admin page |
| ok (short) | settings / status | Run the selected action |
| ok (long press) | home, carousel, any page | Open the settings page |
| ok (long press) | settings | Back to home |
| any judged event | screen off | Lights the screen only. This press is not delivered as an action |

Idle behaviour follows one measured rule set:

- **Auto blank-off** (15 s / 30 s / 1 min / 3 min / always on; factory default 30 s) turns the
  backlight off after that long without a button press. The screen returns to home first, so a
  glance after a blank-off always starts from the same place.
- **Idle deep sleep** happens 5 minutes after the last button press, once the screen is off
  *and* the admin page has been idle *and* no Bluetooth console is connected. A button press
  wakes the device; waking is a reboot, so the device reconnects to Wi-Fi and re-syncs time.
  "Always on" and debug mode both disable this stage.
- The details, including why a connected Bluetooth client keeps the device awake, are in
  [power and idle](docs/development/engineering/power-and-idle.md).

## How a countdown is computed

Each event stores a name, an icon, a kind, a date, an optional category, and how it is shown.
The three kinds behave differently:

| Kind | Behaviour |
| --- | --- |
| Yearly | Folds to the next occurrence: today counts as today, and an event already past this year moves to next year. February 29 folds to February 28 in a common year |
| Once | Uses the full date as given. After that date the countdown shows the days elapsed instead |
| Lunar | Stores a lunar month and day (day 0 means "the last day of that month", for New Year's Eve) and folds it to the next solar date |

The device has no real-time clock. Time arrives from SNTP once it is on Wi-Fi, from the admin
page's "sync with the phone's clock" button, or from the `time` command on either console.
Until the first successful sync the countdowns show a placeholder instead of a number, and
which source was used is printed on the status page. Dates are computed in UTC+8.

The built-in lunar table covers 2018–2050. Outside that range the application says the date
cannot be resolved rather than showing a number that would be wrong.

## The admin page

Open the device's own web page from a phone; it is laid out with the same art and colours as
the device screen, and its preview computes the same countdowns.

| Item | Value |
| --- | --- |
| Hotspot SSID | `LoveCount-XXXX`, where XXXX is derived from the device MAC |
| Hotspot password | Generated once per device and stored in NVS; the device's settings page shows it, and so does the admin page while the hotspot is on |
| Address on the hotspot | `http://192.168.4.1` |
| Address on your LAN | Shown on the device's settings page (DHCP, so it can change) |

The hotspot opens by itself when the device has no saved network. It closes 5 minutes after
the last use, or when you switch it off; a manual off is remembered and the device will not
reopen the hotspot on its own. Clearing all Wi-Fi credentials opens it again, which is the
way back in.

What the page can do:

- **Preview** the home screen and an event card with the current values.
- **Couple**: the start date, both names, and both avatars — either one of the 18 built-in
  pixel icons or one of 4 uploaded photos.
- **Display**: the auto blank-off step, and each event's display mode (list or single page).
- **Events**: add, edit, reorder, collapse, and delete events; pick the icon, the kind
  (yearly, once, lunar), the date, the category, and the display mode. Up to 24 events.
- **Time**: read the device time and its source, and sync the device with the phone's clock.
- **Network and hotspot**: scan, save, and forget networks (up to 5 are remembered, and the
  device picks the strongest one it can see), and switch the hotspot on or off.
- **Bluetooth serial**: enable or disable it.
- **Save** writes the whole configuration to the device.

Uploaded photos never leave the phone: the browser crops them to a square, scales them to
240×240, and reduces them to the 40×40, 16-colour, 4-bit image the device stores. Presets
trade detail for a flatter look, and five knobs expose the same values. The algorithm and its
contracts are documented in [avatar pixelation](docs/development/engineering/avatar-pixelation.md).

## The serial console

The USB Serial/JTAG console is always available; Bluetooth serial is off by default and, when
enabled, advertises the same `LoveCount-XXXX` name and speaks the standard NUS service, so an
ordinary BLE serial app works without configuring UUIDs. Bluetooth turns itself off after
5 idle minutes and never auto-closes while a client is connected.

| Command | Purpose |
| --- | --- |
| `help` | List every command |
| `status` | Time, network, Bluetooth, memory, task stacks, and the current screen |
| `wifi` | Configure Wi-Fi: `wifi`, `wifi list`, `wifi <ssid> <password>`, `wifi del <index\|ssid>`, `wifi clear` |
| `ap` | Background hotspot: `ap`, `ap on`, `ap off` |
| `ble` | Bluetooth serial: `ble`, `ble on`, `ble off` |
| `time` | Read the device time, or write it with `time <unix seconds>` |
| `log` | Log levels |
| `shot` | Dump the current screen over USB as RGB565; `tools/screenshot.py` turns it into a PNG |
| `debug` | Debug mode: keeps the screen on and blocks deep sleep until turned off (USB only) |
| `sleep` | Debug sleep: `sleep light`, `sleep deep [seconds]` (USB only) |
| `key` | Debug input: inject a whole gesture, e.g. `key up`, `key long`, `key dbl up` (USB only) |

Dangerous commands arriving over Bluetooth — changing Wi-Fi, switching the hotspot off, or
writing the time — make the device ask for confirmation on its own screen. Commands typed over
USB do not, because plugging the cable in is already physical contact.

## Factory defaults and privacy

These values apply to a device that has no stored configuration, that is, a fresh one or one
that was factory reset. A device that already has a configuration keeps whatever its owner
chose.

| Item | Factory value |
| --- | --- |
| Start date | 2000-01-01 |
| People | Two placeholder nicknames ("Gugu" and "Gaga"), bird and cat icons |
| Events | New Year's Day, Valentine's Day, the birthday placeholder, National Day, Christmas, and the lunar Spring Festival, Mid-Autumn Festival, and Qixi — all shown in the list |
| Birthday placeholder | Named "GuguGaga", dated **2000-01-01**, yearly, cake icon |
| Auto blank-off | 30 seconds |
| Bluetooth serial | Off |

The factory configuration deliberately carries no real personal data. Both names are
placeholders, and the start date and the birthday entry use the same obviously-fake date,
2000-01-01 — replace them with your own the first time you open the admin page, or delete the
birthday row. Nothing in this repository contains Wi-Fi credentials, device QR secrets, or
uploaded photos; those live in the device's own NVS partition.

## Build, flash, and test

Build with ESP-IDF 5.5.3. The repository gate is the only supported verdict:

```bash
source <path-to-esp-idf-v5.5.3>/export.sh

./tools/validate.sh --static     # repository checks + host tests
./tools/validate.sh --firmware   # isolated ESP-IDF build + merged-image verification
./tools/validate.sh              # the complete gate
```

Flashing the verified merged image from offset `0x0` is the path for a blank device or an
intentional complete refresh:

```bash
python -m esptool --chip esp32c3 -p <port> -b 460800 \
    write-flash 0x0 build/FoloToy-AI-Passport-full.bin

# incremental development: application only, existing stored settings are kept
idf.py -p <port> flash monitor
```

The merged image can reset stored settings; use the segmented `idf.py flash` path when they
must be preserved. The partition table is upstream's minimal default — NVS, PHY data, and one
factory application spanning the rest of the 8 MB flash; see
[firmware layout](docs/development/engineering/firmware-layout.md) and
[build and test](docs/development/engineering/build-and-test.md).

## Where to read more

| Topic | Document |
| --- | --- |
| Upstream platform overview | [docs/README.md](docs/README.md) |
| Working on this firmware with an AI agent | [AGENTS.md](AGENTS.md), [AI development guide](docs/development/ai-guide.md) |
| Board, pins, and buses | [Hardware guide](docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md) |
| Idle, blank-off and deep sleep | [Power and idle](docs/development/engineering/power-and-idle.md) |
| Photo-to-avatar pipeline | [Avatar pixelation](docs/development/engineering/avatar-pixelation.md) |
| Screenshot and key-injection protocol | [Serial screenshot](docs/development/engineering/serial-screenshot.md) |
| Wi-Fi provisioning | [Wi-Fi provisioning](docs/development/engineering/wifi-provisioning.md) |
| Build, test, and firmware layout | [Build and test](docs/development/engineering/build-and-test.md), [Firmware layout](docs/development/engineering/firmware-layout.md) |

## Known limits

- The countdown depends on a synced clock. After a full power cycle, and before the device is
  on Wi-Fi again, date-dependent screens fall back to the last stored snapshot or show a
  placeholder.
- A deep-sleep wake is a reboot: the screen comes back, but Wi-Fi, HTTP, and Bluetooth have to
  come up again.
- The lunar table ends at 2050, and the app reports that limit instead of guessing.
- Dates are computed in UTC+8; there is no timezone setting.
