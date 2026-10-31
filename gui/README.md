# wristctl_gui

The desktop frontend: **App_shell** owns the Wayland window and the input pump,
**Vk_Canvas** draws the UI, and the C core (`wristctl_core`) plus the BlueZ
transport (`wristctl_bluez`) do the protocol work on a worker thread. The same
view and worker run on Android.

The app has no account, cloud sync, analytics or telemetry. Optional SNTP UDP
queries are the only intentional network access; demo mode performs none.

```
src/gui_main.cc         app_shell_main; command line
src/watch_view.cc       shared lifecycle, input, drafts and write actions
src/watch_view_*.cc     focused destinations and detail editors
src/ui_*.cc             app-local navigation, layout, theme and components
src/demo_scenarios.cc   demo-only snapshot fixtures
src/ble_worker.cc       worker thread: scan, connect, handshake, reads and writes
src/sntp_client.cc      validated UDP SNTP queries and UTC conversion
src/settings.cc         route, connection, phone and time-sync preferences
```

## Build

Requires a Vulkan SDK (`vulkaninfo`, `glslc`, `slangc`), the Wayland/xkb
development packages, and the framework submodules:

From the project root:

```sh
git submodule update --init --recursive
./scripts/build.sh build --debug --gui   # -> build/linux_debug/gui/wristctl_gui
./build/linux_debug/gui/wristctl_gui                  # auto-connect
./build/linux_debug/gui/wristctl_gui --no-connect     # recorded demo session
./build/linux_debug/gui/wristctl_gui --timezone Etc/GMT+5
```

or from the project root: `make gui` / `make gui-live`.

The build tool owns the CMake directory: the GUI shares `build/linux_debug`
with the core and CLI. The raw CMake equivalent is still:

```sh
cmake -S . -B build/linux_debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DWRISTCTL_BUILD_GUI=ON
cmake --build build/linux_debug
```

but the builder also probes the GUI prerequisites and sets the test option.

`--timezone ZONE` (or `WRISTCTL_TIMEZONE`) sets the zone used by device-local
**Sync time** and by the Time destination. Without it the host timezone is used. The
flag is applied before the BLE worker starts; SNTP mode always writes UTC.

## Time synchronization

The Time destination selects device-local time or SNTP UTC; SNTP UTC and **Sync on
connect** are the defaults. Servers are tried in the configured order with a
two-second timeout each; the defaults are the four `*.arch.pool.ntp.org`
servers, matching this Linux host's systemd-timesyncd configuration. Edit the
space-separated list to use another NTP server. The worker validates the SNTP
response, displays its server, offset and round-trip time, and never runs
network traffic in demo mode.

A manual SNTP sync reuses the Home/World state read on connection, preserves
both city IDs and unrelated fields, writes UTC to both through `0x1d`, `0x1e`
and `0x1f`, then writes the corrected UTC calendar time with `0x09`. The
GBX-100 returns only slots 0 and 1, so its refresh requests only those slots;
other models retain the five-slot refresh. A valid Home/World result remains
cached if another model's five-city refresh is incomplete, and sync never starts
another DST read. The five-city editor is shown only after a complete five-slot
read. An NTP failure aborts before any watch write.
**Sync on connect** prefetches SNTP for the handshake and performs the same
Home/World UTC setup after connecting; failure skips the connection and retries
with a visible error instead of writing device-local time. The 12H/24H control
uses the existing read-modify-write basic-settings path.

This synchronizes the watch, not the phone or host system clock. `0x09` has no
readback, so verify the displayed time after syncing.

## Connection

The GUI auto-connects on launch when `auto_connect=1` in `gui.conf` (the
default). The worker scans in 10-second windows and retries with a 2, 5, 10,
15, 15-second backoff; the status line shows the attempt and the next retry.
`--no-connect` (or `--demo`) starts on the recorded demo session instead and
does not change the saved preference. `C` and `--connect` turn auto-connect on
and retry immediately. `Disconnect` turns it off and saves that; the Watch page
also has an **Auto-connect: On/Off** button.

While connected, the worker watches `org.bluez.Device1.Connected` and performs a
30-second settings read as a fallback heartbeat. A dropped link clears the
session and starts scanning again without user action. Closing the window
cancels an in-flight scan within about 100 ms.

On Android the same Watch page shows the device scan list, permission banners
and the pair/connect flow; see `android/README.md`. The desktop platform does
not implement a device list, so the pairing panel is hidden there.

The watch advertises only in short windows and stops advertising after a
session. Auto-connect keeps scanning, so press a watch button once if the first
attempts find nothing. When the official app is open on the phone it may hold
the watch; turn off phone Bluetooth or force-stop that app.

The build is Wayland-only. It compiles the font engine's five shader passes plus
Vk_Canvas's overlay/image/shape passes and copies them next to the binary.

## Destinations and editors

Five primary destinations sit in the desktop navigation rail and Android bottom
navigation. Watch, Settings and Diagnostics stay in the utility area rather
than crowding the primary set. Windows narrower than 640 px switch to the same
single-column, bottom-navigation layout as Android.

| Destination | What it shows | Writes |
|---|---|---|
| Overview | glanceable connection, clock, alarm and timer summaries | navigation only |
| Watch | connection, pairing, identity, battery, temperature and MTU | connection preferences |
| Time | local/SNTP UTC, five city/DST editors, watch/app time format | UTC `0x1d`/`0x1e`/`0x1f`, then `0x09`; cities and format |
| Alarms | five readable alarm cards and focused time/mode editors | confirmed `0x15`/`0x16` writes, then readback |
| Timer | six-step sequence, repeat and focused duration/sound editors | confirmed full `0x2A` replacement, then readback |
| Phone | Android forwarding, filters, quiet hours, finder and test detail | `0x30`; no readback |
| Settings | watch preferences/name, app display, Overview cards, capabilities and privacy | `0x13` and optional `0x23` |
| Diagnostics | raw frames/flags, SNTP details and connection log | none |

City, alarm, timer-step and notification-test editors have explicit **Back**
actions. Back and destination changes preserve drafts and pending confirmation.
**Revert** discards the relevant local edits. Alarm, full-timer and city Apply
flows retain their second-tap confirmation and readback. Unread or disconnected
watch data is not presented as an editable zero value.

Overview card visibility is under **Settings → Overview cards**. Training's
supported timer shortcut is under **Settings → Capabilities**; Activity and run
measurement remain explicitly unavailable without fabricated content.

On Android, notification forwarding is opt-in and uses local package and quiet-
hour filtering after the user grants notification access. Phone finder plays a
local alarm for at most 60 seconds and can be stopped manually. See
`android/README.md` for permissions and validation limits.

Activity/convoy download, run-measurement settings and GPS/radio-location upload
remain unavailable until their wire behavior is established. No cloud or online
coaching replacement is used.

Keyboard: `1`-`5` select the primary destinations, `C` connects/enables
auto-connect, `R` refreshes, `T` syncs time, and `Esc` closes an editor/detail
or quits when none is open.

## Demo scenarios

`--demo` (also `--no-connect`) uses the recorded session without hardware or
network access. **Diagnostics → Demo scenarios** selects Recorded,
Disconnected, Scanning, Permission, Empty, Dirty, Busy, Confirmation, Success
and Error presentations. Non-Recorded scenarios are preview-only and block
watch-mutating actions; they do not simulate successful writes. Recorded demo
edits remain local to the recorded session. Scenario selection is absent from
normal sessions.

## Status

The adaptive shell, focused workspaces, demo transport, settings persistence,
scan/connect, handshake, state reads, write paths and auto-connect compile and
run. The desktop demo was exercised at 960×720, 1440×900 and a 480×800 narrow
window, including all eight destinations, alarm draft/confirmation, the
timer-step and watch-name editors, the notification composer and the demo
scenarios. Core packet tests cover writes and notification validation; local
fake-server tests cover SNTP without public network access. Real local-time
write, pairing and state reads were verified separately. SNTP/UTC provisioning,
link-loss reconnect, heartbeat, Android notification/finder integration and the
newer alarm/timer/settings/name/notification writes still require device
validation. Android runtime behavior requires separate on-device verification.
`wristctl.log` beside the binary records BlueZ and transport errors.
