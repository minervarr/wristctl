# wristctl_gui

The desktop frontend: **App_shell** owns the Wayland window and the input pump,
**Vk_Canvas** draws the UI, and the C core (`wristctl_core`) plus the BlueZ
transport (`wristctl_bluez`) do the protocol work on a worker thread. The same
view and worker run on Android.

The app has no account, cloud sync, analytics or telemetry. Optional SNTP UDP
queries are the only intentional network access; demo mode performs none.

```
src/gui_main.cc      app_shell_main; command line
src/watch_view.cc    ten pages, editors, hit-testing, render loop
src/ble_worker.cc    worker thread: scan, connect, handshake, reads and writes
src/sntp_client.cc   validated UDP SNTP queries and UTC conversion
src/settings.cc      page, connection, phone and time-sync preferences
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
**Sync time** and by the Clock page. Without it the host timezone is used. The
flag is applied before the BLE worker starts; SNTP mode always writes UTC.

## Time synchronization

The Clock page selects device-local time or SNTP UTC; SNTP UTC and **Sync on
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

## Pages

| Page | What it shows | Writes |
|---|---|---|
| Home | configurable connection, clock, alarm and timer cards | navigation only |
| Watch | name, model, address, pairing, battery, temperature, MTU | none |
| Clock | local/SNTP UTC, five city/DST editors, 12H/24H | UTC `0x1d`/`0x1e`/`0x1f`, then `0x09`; cities and format |
| Alarms | five editable alarms from `0x15`/`0x16` | both frames, then readback |
| Timer | six-entry program timer from `0x2A` | confirmed full replacement, then readback |
| Training | program-timer shortcut and unsupported-feature status | none |
| Activity | local history status; no fabricated sessions | none |
| Phone | Android forwarding, package/quiet filters, finder, manual composer | `0x30`; no readback |
| Settings | dashboard, display, sound, light, language and watch name | `0x13` and optional `0x23` |
| Console | handshake/read/write log | none |

Drafts preserve fields and unknown bits from the last read. **Revert** discards
local edits. Applying the program timer requires a second tap because the write
replaces all entries and the repeat count. The demo applies edits only to its
recorded session; it never opens a platform link.

On Android, notification forwarding is opt-in and uses local package and quiet-
hour filtering after the user grants notification access. Phone finder plays a
local alarm for at most 60 seconds and can be stopped manually. See
`android/README.md` for permissions and validation limits.

Activity/convoy download, run-measurement settings and GPS/radio-location upload
remain unavailable until their wire behavior is established. No cloud or online
coaching replacement is used.

Keyboard: `1`-`9`, `0` pages, `C` connect/auto, `R` refresh, `T` sync time,
`Esc` quit.

## Status

The window, demo transport, ten pages, settings persistence, scan/connect,
handshake, state reads, write paths and auto-connect compile and run. Core packet
tests cover the writes and notification validation; local fake-server tests cover
SNTP without public network access. The demo exercises the UI without hardware or
network. Real local-time write, pairing and state reads were verified separately.
SNTP/UTC provisioning, link-loss reconnect, heartbeat, Android notification/
finder integration and the newer alarm/timer/settings/name/notification writes
still require device validation. `wristctl.log` beside the binary records BlueZ
and transport errors.
