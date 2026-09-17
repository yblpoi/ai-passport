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
| Admin web page | Network and hotspot card | `main/love_httpd.c`, served over the device's own hotspot. That hotspot only opens while the device has no saved credentials. |

All credential paths converge on `love_net_set_credentials()` and
`love_net_forget()` in `main/love_net.c`, so there is one credential path and one
NVS record. From the console, `wifi` with no arguments prints the current state,
`wifi open <ssid>` joins an open network, and `wifi clear` forgets the
credentials and reopens the hotspot.

The console never logs the password and never reads it back: `love_store_load_wifi()`
is the only reader and the network layer is the only caller. The console parser
splits arguments on spaces, so an SSID or password containing a space has to be
entered from the web page instead.

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

Available commands: `help`, `status` (time, network, Bluetooth, memory), `wifi …`,
`time <unix seconds>` to set the clock, and `ble on` / `ble off`. Once a phone has
connected and subscribed to notifications, the device pushes the output of `help`
to it automatically.

Bluetooth is off by default and its switch lives in the `ble_enabled` field of the
config record. It can be toggled from the device settings page, the admin web page,
or the `ble on` / `ble off` command. While it is on, the console task also watches
for idle time: after **five minutes with nobody connected** it stops the stack and
writes `ble_enabled = 0` back, returning roughly 51 KB of heap. A connected phone is
never dropped automatically.

Stopping the stack has to happen in an ordinary task (`nimble_port_stop()` waits for
the NimBLE host task without a timeout), so `main/love_ble.c` owns a 4 KB console
task that executes commands, decides when to stop, and splits output into
notifications sized to the negotiated MTU. That task is created the first time
Bluetooth starts and stays for the lifetime of the process.
