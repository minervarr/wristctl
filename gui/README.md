# wristctl_gui

The desktop frontend: **App_shell** owns the Wayland window and the input pump,
**Vk_Canvas** draws the UI, and the C core (`wristctl_core`) plus the BlueZ
transport (`wristctl_bluez`) do the protocol work on a worker thread.

```
src/gui_main.cc      app_shell_main; command line
src/watch_view.cc    six pages, hit-testing, render loop
src/ble_worker.cc    worker thread: scan, connect, handshake, reads, time write
src/settings.cc      last page, through arc::fs::layout("wristctl").config
```

## Build

Requires a Vulkan SDK (`vulkaninfo`, `glslc`, `slangc`), the Wayland/xkb
development packages, and the framework submodules:

```sh
git submodule update --init --recursive
cmake -S .. -B ../build-gui -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DWRISTCTL_BUILD_GUI=ON
cmake --build ../build-gui
../build-gui/gui/wristctl_gui              # demo session
../build-gui/gui/wristctl_gui --connect    # scan immediately
```

or from the project root: `make gui` / `make gui-live`.

The build is Wayland-only. It compiles the font engine's five shader passes plus
Vk_Canvas's overlay/image/shape passes and copies them next to the binary.

## Pages

| Page | What it shows | Writes |
|---|---|---|
| Watch | name, model, address, pairing, battery, temperature, MTU, settings | none |
| Clock | host local time | `0x09` time write |
| Alarms | five alarms from `0x15`/`0x16` | none yet |
| Timer | program timer from `0x2A` | none (destructive; deliberately read-only) |
| Notify | explains the `0x30` payload | none yet |
| Console | handshake/read log | none |

Keyboard: `1`-`6` pages, `C` connect, `R` refresh, `T` sync time, `Esc` quit.

## Status

The window, demo transport, six pages, settings persistence, real scan/connect,
handshake, settings/alarms/timer reads and the time write compile and run. The
BLE path has **not** been exercised against a watch in this build: the watch was
not advertising when it was written. `wristctl.log` beside the binary records
the BlueZ/transport errors.

Closing the window while a scan or connect is in progress waits for that call
to return; the transport is blocking by design and has no cancel hook yet.
