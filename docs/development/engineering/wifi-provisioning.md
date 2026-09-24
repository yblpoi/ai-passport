<p align="right">
  <a href="wifi-provisioning.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Wi-Fi Connection via Bluetooth Provisioning

When a user needs the device to connect to a Wi-Fi network, the
[`demo/blufi-provisioning` branch](https://github.com/FoloToy/ai-passport/tree/demo/blufi-provisioning)
provides a reference for Bluetooth-based Wi-Fi setup. The phone supplies the
SSID and password over BLE using BLUFI; the device connects as a Wi-Fi station
and reports connection status. Bluetooth carries the provisioning exchange,
not the application's Internet traffic.

This is an optional application reference. The current `main` Wi-Fi demo only
scans networks; it does not implement connection or provisioning. Do not enable
networking for applications that do not need it.

## Mini program name

Use the companion mini program identified by the
[exact Chinese search name](wifi-provisioning.zh_CN.md#mini-program-name).
Keep that name unchanged in setup instructions rather than inventing a
translated search name. It is the mini program's name, not the device's BLE
advertising name. The reference firmware advertises as `BLUFI_FoloPassport`.

The intended flow is to open provisioning on the device, enable Bluetooth and
the required permissions on the phone, open the mini program, select the target
device, and provide a 2.4 GHz Wi-Fi network's credentials. Confirm that the
device obtains an IP address, then test the application's actual network
request; a successful BLE connection alone does not prove Internet access.

## Code to inspect

- [`main/demo_blufi.c`](https://github.com/FoloToy/ai-passport/blob/demo/blufi-provisioning/main/demo_blufi.c): BLUFI callbacks, Wi-Fi scanning/connection, credential handling, status reporting, and lifecycle.
- [`main/demo_blufi_security.c`](https://github.com/FoloToy/ai-passport/blob/demo/blufi-provisioning/main/demo_blufi_security.c) and its header: BLUFI security negotiation callbacks; do not discard them when extracting the example.
- [`main/demo_radio.c`](https://github.com/FoloToy/ai-passport/blob/demo/blufi-provisioning/main/demo_radio.c): shared initialization of NVS, `esp_netif`, and the default event loop.
- [`main/CMakeLists.txt`](https://github.com/FoloToy/ai-passport/blob/demo/blufi-provisioning/main/CMakeLists.txt) and [`sdkconfig.defaults`](https://github.com/FoloToy/ai-passport/blob/demo/blufi-provisioning/sdkconfig.defaults): source registration, dependencies, NimBLE/BLUFI, and cryptographic configuration. Copying a single C file is not sufficient.

## Integration and acceptance

Start from the application's current baseline, record the reference commit,
and adapt only the relevant networking logic. Do not merge the entire demo or
overwrite current BSP, partitions, or configuration with the branch's older
versions. Implement the application's own UI and keep provisioning state,
tasks, and Wi-Fi/BLE services in the application layer.

Define provisioning entry/exit, timeouts, retry limits, credential persistence,
and an explicit way to clear saved credentials. Never log or commit Wi-Fi
passwords. Preserve LVGL locking, non-blocking callbacks, and cleanup of tasks
and event handlers; budget internal RAM for concurrent Wi-Fi, BLE, and UI use.
The example is not a complete production authorization or security design.

Run the [validation gate](build-and-test.md), then follow the
[on-device testing handoff](../ai-guide.md#offer-on-device-testing). Test with
the named mini program: discovery, successful provisioning, wrong-password and
unavailable-network failures, reconnect/restart behavior, credential clearing,
repeated entry/exit, and an actual application network request. Build success
does not prove mini program compatibility or on-device networking; keep
unperformed checks under `Unverified`.

## Provisioning entry points in this application

This application ships three independent ways to set the Wi-Fi credentials. The
last two share one command set. None of them replaces another.

| Entry | How | Notes |
| --- | --- | --- |
| USB serial console | `wifi <ssid> <password>` | `main/love_console.c`, on the USB-Serial-JTAG console. The only entry that works when the device is not reachable on any network, which is exactly the state a misconfigured device is in. |
| Bluetooth serial console | `wifi <ssid> <password>` in a BLE serial app | `main/love_ble.c` advertises a standard Nordic UART Service, so off-the-shelf apps work. The command set is identical to the USB console. Off by default. |
| Admin web page | Network and hotspot card | `main/love_httpd.c`, served over the device's own hotspot or over the LAN address. Reaching it needs one of those two; see [Hotspot lifecycle](#hotspot-lifecycle). |

All credential paths converge on `love_net_set_credentials()`,
`love_net_forget_ssid()` and `love_net_forget()` in `main/love_net.c`, so there is
one credential path and one NVS record. From the console, `wifi` with no arguments
prints the current state, `wifi open <ssid>` saves an open network, `wifi list`
lists the saved networks, `wifi del <index|ssid>` removes one, and `wifi clear`
forgets everything and reopens the hotspot.

The console never logs the password and never reads it back:
`love_store_load_wifi_list()` is the only reader and the network layer is the only
caller. The console parser splits arguments on spaces, so an SSID or password
containing a space has to be entered from the web page instead.

### Remembering several networks: selection and backoff

The device remembers up to **5** networks (`LOVE_WIFI_MAX`), in save order, in
`s_saved[]` inside `love_net.c`. After boot and after a dropped connection it picks
one like this:

1. **Scan first** (about 1-2 s) and join the strongest saved network it sees;
2. after a candidate fails, prefer the remaining saved networks **the scan saw**
   (strongest first), and only walk them in save order when none of them was seen.
   Save order is the user's own priority, but one list can span several places, so
   working it in order alone would spend the round on networks that have moved away.
   Hidden SSIDs never show up in a scan, which is why the save-order fallback exists
   at all;
3. give each candidate 15 s (`CONNECT_TIMEOUT_MS`), timed from that candidate's own
   start — the comparison is signed on purpose, see below;
4. if a whole round fails, back off and retry: 30 s, doubling each round, capped at
   5 minutes.

That 15 s is measured with `love_net_pick_elapsed_ms()`, never as a plain unsigned
subtraction. The heartbeat samples its `now` at the top of an iteration, and a scan
result can start a candidate later in that same iteration, so `since` is newer than
`now`; unsigned math underflowed into a huge positive number and turned the freshly
chosen candidate into an instant "15 s without an event". The strongest network the
scan had just found was therefore dropped within milliseconds on every round, with
its tried-bit already set, so it was never really attempted at all.

During the backoff it does not reconnect, but each attempt still leaves its reason in
the log: one `love_net` line per failed candidate carrying the Wi-Fi disconnect
`reason` code, one more when a candidate sees no event at all within its 15 s, and a
summary line per round (`ROUND_LOG_MIN_INTERVAL_MS` throttles that one to every 5
minutes in the steady state). That is deliberate: the earlier implementation called
`esp_wifi_connect()` straight from the disconnect event, which with credentials
saved but the network gone meant a reconnect every second plus one warning each
time. For driver-level detail beyond that, raise the driver logs with `log wifi info`.

Credential edits: an existing SSID updates its password in place and keeps its
position, a new name is appended, and a full list means you delete one first (web
page or `wifi del`). Editing the network the device is currently on reconnects;
editing any other one leaves the live connection alone — adding a spare network
should not kick the device off the one it is using.

### Hotspot lifecycle

The hotspot is `LoveCount-XXXX`; its SSID is derived from the same MAC and its password
is **generated randomly on first boot and stored in NVS** (`love_store_load_ap_pass()`,
shown on the device screen). It used to be derived from the MAC as well, which meant
anyone who could see the SSID could compute the password and then reach the admin page,
which has no authentication of its own. It opens automatically in exactly three situations:

| Trigger | Condition | Code |
| --- | --- | --- |
| Boot | The device has no saved credentials | `love_net_init()` |
| Deep-sleep wake | The hotspot was on when the device went to sleep, and no manual-close gate is set | `love_net_deinit()` records it in RTC memory, `love_net_init()` restores it |
| Station-down fallback | Credentials exist but the station has neither connected nor been in a selection round for 60 s | `love_net_poll()` |

Deep sleep is a restart, so the "the user wanted this hotspot" intent only survives in RTC
memory; without that snapshot a device with saved credentials would come back with the
hotspot off until the 60 s fallback fires — and never, once the manual-close gate below is
set. The gate always wins over the snapshot.

The fallback timer only runs while no attempt is in flight **and no scan is pending**:
a round of candidates takes tens of seconds, and switching to APSTA in the middle of
one restarts the radio (`esp_wifi_set_mode()`), which would throw that round away.
The same switch invalidates an in-flight scan, and a round that starts out by losing
its scan degrades to blind save order. A scan takes 1-3 s, so waiting for it is cheap.

It also opens on demand from the device settings page, the network card of the
admin page, or the `ap on` console command. While it is open and the station is
connected it closes itself after **five minutes without activity** (every admin
page request and every button press on the device counts); that automatic close
deliberately does **not** count as a manual close, so the fallback above still
works later.

Opening or closing it walks `apply_mode()`, which is the only place the driver mode is
changed. That call reports errors instead of aborting the firmware: it runs on the input
task when the settings row is pressed, which can collide with a running scan, and an
`ESP_ERROR_CHECK` there used to reboot the device — from the outside, "I pressed the
hotspot row and nothing happened". A failed switch now logs once and is retried on the
1 Hz heartbeat (`s_mode_dirty`), so a rejected press still takes effect about a second
later. Each press leaves one line in the log (the module logs opening and closing the
hotspot separately), which is the only way to tell "the key never reached the network
layer" from "the mode switch itself failed".

Switching modes restarts the radio, so a connected station drops for a moment when the
hotspot is opened or closed — `love_net` logs one dropped-connection line with reason 8 —
and comes back through a normal selection round about two seconds later. That is the
driver's behavior, not a defect: `esp_wifi_set_mode()` cannot add or remove the AP
interface without bringing the station down with it.

Closing it from the device, from the web page or with `ap off` sets a
*manual-off* latch, persisted in the `ap_off` key of the same NVS namespace.
While the latch is set, both automatic paths are skipped: the hotspot does not
come back behind the user's back, not even after a reboot or a deep-sleep wake.
`ap on` and `wifi clear` clear the latch — `wifi clear` does so because removing
the credentials has to leave a provisioning entry point. With the latch set and
the station down, the ways back onto the network are the device settings page and
the two serial consoles.

### Bluetooth serial console

`main/love_ble.c` serves the same command table as the USB console
(`main/love_console.c`) over a standard Nordic UART Service, so off-the-shelf
apps (Serial Bluetooth Terminal, nRF Connect) work without any UUID setup. The
device advertises as `LoveCount-XXXX`, where `XXXX` comes from the Bluetooth MAC.

| Role | UUID |
| --- | --- |
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| Phone to device (write) | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| Device to phone (notify) | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

The 128-bit UUID is deliberately kept out of the advertising packet: the 31-byte
packet cannot hold the flags, the device name and a 128-bit UUID, and
`ble_gap_adv_set_fields()` returns `EMSGSIZE` when it does not fit.

Available commands: `help`, `status` (time, network, Bluetooth, the screen you are
on and its page number, memory, the task stack watermarks, the LVGL pool and the
event order), `wifi …` (`list` prints the saved networks, `del <index|ssid>`
removes one),
`ap` (hotspot state, `ap on`, `ap off`),
`time <unix seconds>` to set the clock, `ble on` / `ble off`,
`log` (read or set log levels: `log warn` for the global level, `log wifi info` for
one tag, `log reset` back to the defaults — the default policy already pushes the
`wifi` and `wpa` driver tags down to warn, see
[selection and backoff](#remembering-several-networks-selection-and-backoff)),
and the **USB-only** `shot` (see [serial-screenshot.md](serial-screenshot.md)),
`key` (inject a whole button gesture: press, long press or double press), `sleep` (trigger light/deep sleep, for checking the
idle behaviour) and `debug on`. Once a phone has connected and subscribed
to notifications, the device pushes the output of `help` to it automatically.

#### Connecting from a computer

`tools/ble_console.py` is a terminal for the same service, so a laptop with a
Bluetooth adapter can be used for debugging without a phone or the hotspot:

```bash
pip install bleak
python3 tools/ble_console.py                 # picks the first LoveCount-* device
python3 tools/ble_console.py LoveCount-8C5E  # by advertised name
printf 'status\n' | python3 tools/ble_console.py   # non-interactive
```

Bluetooth has to be on before it can be found (see below), and the tool reports
rssi while scanning so a weak link is obvious.

Bluetooth is off by default and its switch lives in the `ble_enabled` field of the
config record. It can be toggled from the device settings page, the admin web page,
or the `ble on` / `ble off` command. While it is on, the console task also watches
for idle time: after **five minutes with nobody connected** it stops the stack and
writes `ble_enabled = 0` back, returning roughly 51 KB of heap. A connected phone is
never dropped automatically.

Stopping the stack has to happen in an ordinary task (`nimble_port_stop()` waits for
the NimBLE host task without a timeout), so `main/love_ble.c` owns a 4 KB console
task that executes commands, decides when to stop, and splits output into
notifications sized to the negotiated MTU. That task is created *before*
`nimble_port_init()` — once NimBLE is up the heap is too fragmented for a
contiguous 4 KB stack — and it exits again when Bluetooth is stopped, so the stack
goes back to the system heap. Keeping it alive permanently cost far more than it
looks: on this device the largest contiguous free block fell from 69,632 to 34,816
bytes after Bluetooth had been used once, and the Wi-Fi driver needs a large block
to send a frame.

### Security of this console

The link itself is **not authenticated**: the NUS characteristics carry no
`_WRITE_ENC` / `_AUTHEN` flags (`main/love_ble.c`) and no NimBLE security-manager
fields are configured, so any central that connects may write commands without
pairing. The protections are therefore in the console and in what the commands can
reach:

| Protection | Why |
| --- | --- |
| Bluetooth off by default, and the stack stops itself after five minutes with nobody connected | There is no link left to attack unless the owner turned it on |
| `key`, `sleep`, `shot` and `debug on` are **USB only** | They would otherwise hand a nearby stranger a remote control for the UI, a way to force the device to sleep, or a way to keep it awake forever |
| `wifi …`, `ap off` and `time <seconds>` require **on-device confirmation** when they arrive over Bluetooth — the screen shows what is being requested and waits up to 8 s for a press of the OK button (long-press rejects; timeout rejects) | These change persisted state or can lock the owner out. USB is exempt: holding the cable *is* physical presence |
| The hotspot password is random per device and stored in NVS (shown on the screen) | It used to be derived from the MAC, so anyone who could see the SSID could compute it and then use the unauthenticated admin page |

Known residual risks, deliberately not closed yet: `status` still prints the
configured SSID, the LAN address and the owner's own event/category names; an
attacker can still hold the single connection open to keep the radio (and ~51 KB of
heap) busy and to block the owner; and there is no link-layer pairing, because
requiring it costs real compatibility (iOS only pairs when a characteristic demands
encryption, Android BLE terminals handle PIN pairing inconsistently, and
`bleak` cannot pair at all on macOS).
