<p align="right">
  <a href="power-and-idle.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Power, blank-off and idle deep sleep

This application is a mains-or-battery desk ornament with one screen and three
buttons. Its idle behaviour is a small state machine whose details were measured on
the real board, not assumed. The deep-sleep stage deliberately follows the level of
the upstream reference implementation — the "Sound Effects Keychain" firmware, which
"*slept after five idle minutes and woke on a button*"
(`docs/reference/shinku-chen/deep-sleep-peripheral-power-off.md`) — and stops there:
nothing here tries to squeeze the standby current below what that firmware does.

## The idle stages

| Stage | Trigger | What happens | Code |
| --- | --- | --- | --- |
| Blank off | No **button press** for `blank_off_seconds` (15 s / 30 s / 1 min / 3 min, or "always on" = never) | The screen returns to the home screen **first**, then the backlight goes off | `screen_off()`, `blank_off_poll()` in `main/love_app.c` |
| Deep sleep | `IDLE_DEEP_SLEEP_S` (**300 s**, the official five idle minutes) since the last button press, screen already off, and nobody using it (see below) | Wi-Fi/BLE/HTTP are stopped, the wake source is armed, then the peripherals are shut down in a fixed order and the chip sleeps **until a button is pressed** | `love_app_idle_poll()` → `love_app_sleep_deep(0)` → `power_sleep_deep_for(0)` → `run_deep_sleep()` |
| Debug mode | `debug on` over USB (persisted in NVS) | **Neither stage above happens** until `debug off` | `love_app_set_debug()`, the `debug` command in `main/love_console.c` |

Note the asymmetry in the first two rows: the **blank-off counts button presses only**,
while the deep sleep additionally waits for the web and Bluetooth links to go quiet.
That is deliberate: a phone with the admin page open (it polls every 10 s) should not
keep the screen lit, but it must not be cut off by a sleep either. "Nobody using it"
therefore means `love_httpd_client_idle_seconds() >= IDLE_DEEP_SLEEP_S` **and** no
connected Bluetooth console (`love_ble_connected()`). The consequence of the Bluetooth
clause is worth stating: a client that connects and never disconnects keeps the device
awake indefinitely, which matches the Bluetooth idle-off rule ("never auto-close while
a phone is connected").

Three more rules that are easy to get wrong:

- **The first key press after blank-off only wakes the screen.** It is not delivered as
  a key action, otherwise "glance at the countdown" would also navigate or change a
  setting. The rule lives in `main/love_key.h`: one physical press emits several events
  (PRESS on the way down, then the component's judged event — CLICK, DOUBLE or LONG —
  after the key is released), and only the judged event counts as "what the user wants
  to do". PRESS is not an action and does **not** wake the screen either: with PRESS
  waking, the judged event of the same gesture lands on an already lit screen and runs.
  That is exactly how this broke on the device — the screen came on *and* changed page —
  and the console `key` command could not reproduce it, because it injected a single
  CLICK (it now injects a whole gesture, see
  [serial-screenshot.md](serial-screenshot.md)).
  This rule is about a blank-off *within the same run*; a deep-sleep wake is a
  fresh boot, so there is no "was the screen off?" state to consult — see the note under
  "Waking up again".
- **Blank-off always returns to the home screen**, because the blank-off is usually
  followed by a deep sleep and therefore by a reboot: the first screen after a wake
  must be the "days together" screen, not whatever card was left open.
- **"Always on" (`blank_off_seconds == 0`) disables the deep sleep too.** Someone who
  explicitly asked for a lit screen did not ask for the device to go to sleep.

## Waking up again

The idle path arms **exactly one** wake source: any of the three buttons.

| Source | Configured in | Notes |
| --- | --- | --- |
| Any of the three buttons | `arm_button_wake()` in `main/power_sleep.c`: `gpio_config()` the pin as an input, then `esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW)` | All three keys share the ADC node GPIO0 whose pressed voltages are 0 / 300 / 595 mV and whose released level is 3300 mV, so one *low-level* wake covers all three |
| RTC timer | Only the status-page action and `sleep deep <seconds>` | Deliberately **not** used by the idle path |

Why the idle path has no timer — this is our reasoning, not a quotation: a timer wake
makes the device reboot on a schedule, which is a reboot cycle rather than a power-off,
and it would also mean the device wakes (and reconnects) every few minutes for nothing.
The upstream reference puts the same idea in its own words in
`docs/reference/shinku-chen/display-refresh-and-deep-sleep.md` ("a timer-only wakeup
causes a periodic reboot cycle, not a true 'power off'; use GPIO wakeup when the intent
is to sleep until a user action"). Sleeping until a button is pressed is what lets the
idle timer be as long as the official five minutes; the previous revision could not do
that, because with no button wake it had to re-wake every 300 s just so a button would
eventually appear to work.

Because a deep-sleep wake **is a reboot**, the first thing the user sees is the normal
boot sequence, and a key that is still held down when the button driver starts can
produce an ordinary event (e.g. a long press opening the settings page). An ordinary tap
(~120 ms) is over long before that, so this only affects a deliberately held key.

## Arming the button wake, and what it took to make it reliable

Three things have to hold at the same time; the contract test asserts the ordering, and
the comments in `power_sleep.c` carry the details:

1. **Detach the sampling and the ADC input network first** (`bsp_button_suspend()`),
   because the wake source watches the *level* of this very node. The order
   (`bsp_button_suspend()` before `arm_button_wake()`) is asserted by
   `tests/test_deep_sleep_contract.py`.
2. **No internal pulls on this node.** It already has the board's external 10 kΩ
   pull-up, and `bsp_pins.h` warns that the internal ~45 kΩ one would shift all three
   key levels. `arm_button_wake()` therefore configures the pin as a plain input with
   both internal pulls disabled.
3. **`CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS=n`** in `sdkconfig.defaults`.
   IDF's `gpio_deep_sleep_wakeup_prepare()` otherwise adds an internal pull by itself
   before sleeping, and `esp_sleep.h` warns about exactly that combination: "*when using
   external pull-up or pull-down resistors, please be sure to disable the
   ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS option, as the combination of internal and
   external resistors may cause interference*". (For a *low-level* wake the pull IDF
   would add is a pull-**up** — the same direction as the board's external one — so this
   option is not a plausible explanation for a false low-level wake; it is the vendor's
   documented requirement for a board that has its own resistors, and it leaves exactly
   one source of truth for the pin's sleep-time state.)

The "sleeps and wakes itself after a few seconds" symptom seen in an **earlier** revision
of this firmware (2–30 s into every deep sleep, with the screen going dark and the device
restarting) was never reproduced under a controlled measurement, and the two explanations
recorded at the time were both about the *observer* rather than the board: a software
press-filter that slept the device back after each wake (removed — see below), and the
host resetting the chip by opening the USB port while looking at it. Note also that the
arming call in the revision that was in the tree sat behind `#if 0`, so it never actually
armed anything. What can be stated as measured is the result of the current combination
(ordering, pull configuration, `sdkconfig` line): a button-only deep sleep held for
**202 s with zero spurious wakes**, and a real key press woke it (see the table below).

### If the wake source cannot be armed, do not sleep

`esp_deep_sleep_enable_gpio_wakeup()` takes a **pin bitmask**. An empty mask is the
nastiest case: on ESP-IDF 5.5.3 it does **not** fail (the invalid-pin check finds nothing
to reject, the loop never runs, and the function returns `ESP_OK`), so the chip would
sleep with nothing armed and no error to check — "it sleeps but won't wake". That case is
therefore excluded at compile time (`_Static_assert` on a non-empty mask), while an
invalid (non-RTC) pin still surfaces as an error return, which is propagated up to
`run_deep_sleep()`. A failure to arm means the device **does not sleep**: it re-attaches
the buttons (restarting if that fails) and reports the error. `love_app_sleep_deep()`
waits for `power_sleep_committed()` — the worker sets it once the wake source is armed,
i.e. once the failing part is behind it — and brings Wi-Fi/BLE/HTTP back if it never
appears, because otherwise the device would be left in the worst state: network stopped
and no sleep.

### Do not try to validate the press after waking

It is tempting to filter spurious wakes in software: sample the pin right after a GPIO
wake and go back to sleep if it does not read low. That was implemented and then removed
because it breaks the normal case: the bootloader takes **~350 ms** to reach
`app_main()`, while an ordinary tap lasts about 120 ms, so by the time the check runs the
button has already been released. The symptom was "you have to press the button several
times before it wakes up". Note also that the earlier "slept the full 104.7 s" reading
came from that very filter, which slept the device back after each spurious wake and was
invisible to a 1-second port poll — a filter can make a broken wake look healthy, so it
is removed at the source instead.

Also worth knowing: **USB-Serial-JTAG is powered down in deep sleep**, so the serial
port disappears and a host cannot wake the device.
That is what debug mode is for.

## Debug mode

`debug on` (USB only) keeps the screen on and suppresses both idle stages until
`debug off`. It is stored in NVS so flashing/rebooting in the middle of a session does not
silently re-enable sleeping, and the home screen hint changes to a debug-mode hint (screen
stays on, no deep sleep) so an accidentally left-on device is obvious. Turning it *off* is
allowed from any link (Bluetooth included): leaving the mode only restores the default
behaviour, and it resets the idle timer so that the device does not blank-and-sleep the
instant the command returns.

## Measured on the real board (2026-09-17, ESP32-C3 rev v1.1)

| Quantity | Value |
| --- | --- |
| Idle path | Blank-off at the 30 s setting; the serial port (i.e. the whole chip) went away on its own **324 s** after the last activity, with nothing touching the device |
| Spurious wakes | Zero over a 202 s button-only deep sleep, and again over the 324 s idle-path observation (both probed with `ls`, never opening the port) |
| Button wake | A physical tap on any key brought the port and the app back; `status` reports the wake cause as a button wake, also after the internal pull-up was removed from the wake pin |
| Timer wake | `sleep deep 20` → the port returned at **20 s**, wake cause reported as a timer wake |
| Wake source | Buttons only for the idle path; no timer |
| Deep-sleep teardown order | CW2017 → ES8311 → I2S → shared I2C → ST7789, plus arm-then-check ordering, asserted by `tests/test_deep_sleep_contract.py` |
| Manual light sleep (`sleep light`) | Still works: codec suspended/restored, screen off/on, screen returns |
| Debug mode | `debug on`: after 92 s of no input (blank-off setting 30 s) the screen was **still lit**; `debug off` reset the idle counter to 2 s |
| Free heap before the sleep request | ~79.6 KB (largest contiguous block 65,536) |
| NVS across a flash | Preserved: no "config record version/length mismatch" line, Wi-Fi credentials and settings intact |

### Observing deep sleep without fooling yourself

**"Why is it not sleeping?" is answered by one line.** `status` prints the four fields
that decide it (the device prints them in Chinese; the order is fixed):

| Field | Example | Meaning |
| --- | --- | --- |
| Button idle / threshold | `312 s` / `300` | Has the button been quiet for long enough? |
| Blank-off setting | `30 s` (or "always on") | "Always on" disables the sleep **on purpose** and is printed as such |
| Web idle | "never requested", or `12 s` | A phone with the admin page open polls every 10 s, so this never grows |
| Bluetooth | "connected" / "none" | A connected console session blocks the sleep |

In practice the two that catch people out are the phone with the admin page open and a
blank-off setting of "always on".

Two more traps when *watching* it:

- Opening the serial port **can reset the chip** (`rst:0x15 USB_UART_CHIP_RESET`) when the
  host's DTR/RTS lines change state, which makes "why did it wake up?" impossible to answer
  and makes the wake cause read back as a plain reset. **Every port open also restarts the
  idle timer**, so a host that keeps opening the port (a script, a monitor, an AI agent
  poking at the device) can keep the device awake indefinitely and make the idle path look
  broken. Verify the idle path with a probe that does not open the port — `ls /dev/cu.usb*`.
- To read the wake cause afterwards, `status` prints both the live cause and (when the
  live one is already a plain reset) the reason recorded in RTC memory by the last real
  deep-sleep wake.
- After a *light* sleep the wake-cause register reads "timer" as well, because
  `esp_sleep_start` shares that register. Check the uptime field before concluding that a
  deep sleep happened.
