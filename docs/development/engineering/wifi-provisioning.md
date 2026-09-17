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

All credential paths converge on `love_net_set_credentials()` and
`love_net_forget()` in `main/love_net.c`, so there is one credential path and one
NVS record. From the console, `wifi` with no arguments prints the current state,
`wifi open <ssid>` joins an open network, and `wifi clear` forgets the
credentials and reopens the hotspot.

The console never logs the password and never reads it back: `love_store_load_wifi()`
is the only reader and the network layer is the only caller. The console parser
splits arguments on spaces, so an SSID or password containing a space has to be
entered from the web page instead.

### Hotspot lifecycle

The hotspot is `LoveCount-XXXX`; its SSID is derived from the same MAC and its password
is **generated randomly on first boot and stored in NVS** (`love_store_load_ap_pass()`,
shown on the device screen). It used to be derived from the MAC as well, which meant
anyone who could see the SSID could compute the password and then reach the admin page,
which has no authentication of its own. It opens automatically in exactly two situations:

| Trigger | Condition | Code |
| --- | --- | --- |
| Boot | The device has no saved credentials | `love_net_init()` |
| Station-down fallback | Credentials exist but the station has not been connected for 60 s | `love_net_poll()` |

It also opens on demand from the device settings page, the network card of the
admin page, or the `ap on` console command. While it is open and the station is
connected it closes itself after **five minutes without activity** (every admin
page request and every button press on the device counts); that automatic close
deliberately does **not** count as a manual close, so the fallback above still
works later.

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
event order), `wifi …`,
`ap` (hotspot state, `ap on`, `ap off`),
`time <unix seconds>` to set the clock, `ble on` / `ble off`,
and the **USB-only** `shot` (see [serial-screenshot.md](serial-screenshot.md)),
`key` (inject a button press), `sleep` (trigger light/deep sleep, for checking the
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
