# wristctl project instructions

Authoritative for `wristctl/`. Workspace routing and the G-SHOCK hardware rules
live in `../AGENTS.md`; read that before touching a watch.

## Shape

```
libs/firstparty/wristctl_core/   portable C11 protocol library (the product)
  include/wristctl/              public headers (session.h is hand-written)
  src/                           codecs + model registry + session state machine
  tests/                         assert() suites, shared vectors
  spec/                          hand-authored source of truth
  gen/                           extractors and generators (dev tooling)
libs/firstparty/framework/       App_shell + Vk_Canvas submodules (GUI only)
libs/firstparty/archive_engine/  arc_fs/arc_util submodule (GUI paths only)
assets/fonts/                    minervarr/fonts submodule (ui/ui.otf)
platform/linux/                  BlueZ transport (C11 + sd-bus)
gui/                             App_shell/Vk_Canvas frontend (C++17)
py/                              Python reference implementation + GTK prototype
android/                         NativeActivity frontend: same gui/ WatchView + BleLink.java
tools/                           capture decoder, probe, vector authoring
docs/                            screenshots from the demo transport
```

`libs/firstparty` mirrors the workspace convention: each library is a
self-contained CMake project, and frontends link it rather than compiling its
sources. `libs/thirdparty` is the place for vendored code.

Mission state: read `../.opencode/context.md`, `../.opencode/status.md`, and
`../.opencode/todo.md` before changing anything.

## Build and test

`scripts/build.sh` is the front end; CMake + Ninja is what it drives. Build
output is one `build/` root with one directory per variant:
`build/linux_debug`, `build/linux` (portable Release), `build/linux_native`
and `build/linux_custom-<arch>[_debug]`. `make` is a thin wrapper.

```sh
./scripts/build.sh test              # generate, Debug, ctest, Python suites
./scripts/build.sh build --debug     # build/linux_debug (GUI auto-detected)
./scripts/build.sh build --release   # build/linux
./scripts/build.sh build --release --arch native  # build/linux_native
./scripts/build.sh build --dry-run   # print the cmake commands
./scripts/build.sh --version

make test          # same as ./scripts/build.sh test
make test-tool     # the build tool's own unit tests (no toolchain)
make generate      # regenerate the Python and C codecs from spec/
make check         # test + test-tool
make gui           # build/linux_debug with the GUI; auto-connect
make gui-live      # same, then force an immediate scan
make ui-demo       # GTK4 prototype against the recorded session
make android       # APK; links the same wristctl_core target
```

Direct equivalent, when the build tool is not wanted:

```sh
cmake -S . -B build/linux_debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DWRISTCTL_BUILD_TESTS=ON -DWRISTCTL_BUILD_GUI=OFF
cmake --build build/linux_debug
ctest --test-dir build/linux_debug --output-on-failure

cmake -S . -B build/linux_debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DWRISTCTL_BUILD_TESTS=ON -DWRISTCTL_BUILD_GUI=ON
cmake --build build/linux_debug
./build/linux_debug/gui/wristctl_gui
```

Tests are plain `assert()` binaries registered with CTest and built Debug-only.
They are narrow: link the core target, never a frontend.

## Platform and framework libraries

- `platform/linux/` is `wristctl_bluez`: C11 + sd-bus, no libdbus, no shelling
  out to `bluetoothctl`. It is single-threaded and belongs on the GUI worker
  thread. It exposes `wc_bluez_pump`, `wc_bluez_link_lost` and
  `wc_bluez_set_cancel`, which the worker uses for idle link-loss detection and
  for aborting a scan when the window closes.
- `libs/firstparty/framework/App_shell` and `Vk_Canvas_Lb_LAW` are submodules.
  They are C++17/Vulkan. The Linux GUI is gated behind `WRISTCTL_BUILD_GUI=OFF`
  because it needs the Vulkan SDK and Wayland; Android uses the same libraries
  through `AndroidHost` and `android/CMakeLists.txt`.
- `libs/firstparty/archive_engine` is a submodule used only by the GUI, through
  `arc_fs::layout("wristctl")` for its settings file. Never link it into the C
  core: `arc::Result` is C++ and cannot cross the JNI/C ABI.
- Vk_Canvas and archive_engine are AGPL-3.0; App_shell ships no LICENSE file
  upstream. The project is AGPL-3.0; see `THIRD_PARTY.md` for the open item.

## Rules that outlive any task

- `libs/firstparty/wristctl_core/spec/*.json` and `spec/vectors/*.json` are
  hand-authored. Generated files are:
  `py/wristctl_proto/generated/*`,
  `libs/firstparty/wristctl_core/include/wristctl/{generated,models}.h`,
  `libs/firstparty/wristctl_core/src/{generated,models}.c`, and
  `libs/firstparty/wristctl_core/tests/generated_vectors.h`.
  `include/wristctl/session.h` and `src/session.c` are hand-written.
  Edit the spec, then run `make generate`.
- The core is C11 with no OS headers, no JNI and no platform `#ifdef`s. A
  transport or filesystem need belongs in a frontend or a platform library, not
  in `wristctl_core`.
- A protocol claim needs a source: a capture frame or a decompiled method. Mark
  derived vectors as derived in their `source` field.
- Android and the desktop GUI compile/link the same core target. Never copy the
  core into a frontend.
- Pairing and GBX-100 reset-state quirks are recorded in
  `../GSHOCK_DECOMP/work/analysis/pairing.md`; read it before changing
  `session.c` or `platform/linux/`.
- Firmware update stays out of scope. It needs vendor binaries and can brick
  the watch.
- License: AGPL-3.0. The GUI links AGPL-3.0 framework libraries.
