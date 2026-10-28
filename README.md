# wristctl

A free-software replacement for the CASIO G-SHOCK MOVE watch-control stack.
The wire protocol is described once in
`libs/firstparty/wristctl_core/spec/`, and the Python and C codecs are
generated from that description.

Status: **protocol skeleton, C++ GUI, GTK prototype, Android probe app**. The
C11 core is the product; `gui/` is the App_shell/Vk_Canvas desktop frontend and
`android/` is the JNI probe app. The Python tree remains the reference
implementation and the hardware harness. The C++ GUI and the GTK app have been
exercised against the recorded session, not yet against a watch. The Android
app has been installed and its JNI path verified on a phone; its BLE path has
not been run against a watch yet.

## What is validated

`make test` regenerates every codec and runs the same 20 golden vectors through
both implementations:

- 137 watch models, 37 protocol generations, 107 capability flags extracted
  from the vendor app's `CasioLibUtil.DeviceType` table
- 13 frame classes covering the connection handshake and the settings that were
  proven on a GBX-100: time, basic settings, five alarms, program timer, watch
  name, watch condition, DST state/setting, city name, app info, service
  discovery and error frames
- 4 CTest suites (vectors, models, session replay, C++ consumer) and 43 Python
  tests, all passing

The Android app (`android/`) compiles those same C sources into
`libwristctl.so`, so the codecs under test are the codecs that ship.

The vectors come from the recorded HCI session and the hardware probes in
`../GSHOCK_DECOMP/work/`; derived vectors say so in their `source` field.

## Layout

```
libs/firstparty/wristctl_core/
  spec/classes.json      frame layouts (the source of truth)
  spec/models.json       generated model/capability registry
  spec/vectors/*.json    golden vectors, shared by Python and C
  gen/                   extractors and generators
  include/ src/ tests/   C11 core: codecs, model registry, session state machine
libs/firstparty/framework/   App_shell + Vk_Canvas submodules (GUI)
libs/firstparty/archive_engine/  arc_fs/arc_util for GUI paths (submodule)
platform/linux/          BlueZ transport over sd-bus (C11)
gui/                     App_shell/Vk_Canvas desktop frontend (C++17)
py/                      Python reference implementation (prototype) + GTK app
android/                 single-activity app; links wristctl_core over JNI
tools/                   capture decoder, vector authoring, hardware probe
```

Generated files are `py/wristctl_proto/generated/*`,
`libs/firstparty/wristctl_core/include/wristctl/{generated,models}.h`,
`libs/firstparty/wristctl_core/src/{generated,models}.c` and
`libs/firstparty/wristctl_core/tests/generated_vectors.h`.
`include/wristctl/session.h` and `src/session.c` are hand-written.
Edit the spec, not the generated files.

## Commands

```sh
make test        # regenerate, configure Debug, build, ctest, then Python suites
make generate    # regenerate only
make configure   # cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
make build       # configure + ninja
python3 tools/decode_capture.py   # coverage report against the recorded handshake
python3 tools/make_vectors.py     # rewrite the core's spec/vectors from the authored list

make gui         # configure build-gui with WRISTCTL_BUILD_GUI=ON and run the demo
make gui-live    # same, then scan immediately (--connect)
make ui-demo     # GTK prototype against the recorded session
make ui          # GTK prototype against a real watch

make android          # build app/build/outputs/apk/debug/app-debug.apk
make android-install  # build, then adb install
make android-lint     # lintDebug
```

The GUI needs a Vulkan SDK (`vulkaninfo`, `glslc`, `slangc`) and the
Wayland/xkb development packages. The framework libraries are submodules:

```sh
git submodule update --init --recursive
```

Details and page status are in `gui/README.md`.

The Android build needs JDK 17+, SDK 36 and NDK 29.0.14206865;
`android/local.properties` holds the SDK path on this machine. Details and the
native-activity migration notes are in `android/README.md`.

Tests need Python 3.10+, a C11 compiler and a C++17 compiler (the C++ consumer
test proves the headers are usable from C++).

### C++ GUI

`gui/` is the App_shell + Vk_Canvas frontend. It needs the Vulkan SDK
(`vulkaninfo`, `glslc`, `slangc`) and the Wayland/xkb development packages, and
it renders with the `assets/fonts` submodule (`ui/ui.otf` copied beside the
binary):

```sh
git submodule update --init --recursive
make gui          # build-gui with WRISTCTL_BUILD_GUI=ON; demo session
make gui-live     # same, then scan immediately
```

The worker thread owns BlueZ and the C session, so the window never blocks on
D-Bus. `gui/README.md` lists what each page can write; alarms and the program
timer are read-only in this build, and notifications are not wired yet.

![C++ GUI on the recorded demo session](docs/gui-watch.png)

### GTK prototype and hardware probe

The Python app and the hardware probe need `bleak`; the app additionally needs
PyGObject/GTK4:

```sh
# Debian/Arch: install python-gobject and gtk4 first.
uv venv --system-site-packages --python 3.14 .venv
uv pip install --python .venv/bin/python bleak

make ui-demo                     # recorded session, no watch
make ui                          # real watch
PYTHONPATH=py .venv/bin/python -m wristctl_proto.ui --demo --screenshot /tmp/ui.png

PYTHONPATH=py .venv/bin/python tools/probe.py --scan
PYTHONPATH=py .venv/bin/python tools/probe.py --wait 90          # robust: scan by name
```

Scan by name rather than address. The GBX-100 regenerates its static random
address on power-up: it was `DA:02:44:21:46:51` during the October capture and
`EA:02:B4:35:46:51` later the same day. Bonding is per address, so a rotating
address also explains why the vendor app avoids mandatory bonding on this model.

The window has Watch / Clock / Alarms / Timer / Notify / Console pages. The
watch advertises briefly, so Connect keeps scanning for up to 90 s and asks you
to press a watch button.

| | |
|---|---|
| ![Watch](docs/watch.png) | ![Clock](docs/clock.png) |
| ![Alarms](docs/alarms.png) | ![Timer](docs/timer.png) |

`probe.py` connects, runs the identify/configure handshake, answers `0x47 02`
with a bond (or a Link Loss Alert write for models that use it), waits for
`0x47 01`, and prints the model, condition and bond state. It is read-only apart
from the app-info write and the pairing step.

## Spec schema

A class is a fixed-length frame whose first byte is the class id:

```json
{ "id": 9, "name": "current_time", "direction": "write",
  "confidence": "observed", "source": "…",
  "length": {"min": 11, "max": 11},
  "fields": [ {"name": "year", "type": "u16le", "offset": 1} ] }
```

Field types: `u8`, `i8`, `u16le`, `bcd8`, `bit` (a mask inside a `u8`
container), `bytes`, `string` (UTF-8, NUL-padded) and `array` (fixed count and
stride, item fields relative to the item).

A field with `"expected": N` is a constant: it is checked on decode, written on
encode, and omitted from both structs. That is how fixed bytes such as the
alarm `0x40` separator stay correct without every caller setting them.

Bit fields do not replace their container: the container byte is preserved and
the bits are applied on top, so unknown bits survive a read-modify-write.

## Findings recorded while building this

- Capability flags are mostly **generation-wide**, not per model. `isEqualsModel()`
  compares the generation index, so `GBX-100` shares flags with `GBD-H1000`; only
  `this == MODEL` checks split a generation. Tide is the clearest example: it is
  `this == GBX_100` and applies to exactly one model.
- The vendor app identifies a model by **substring of the advertised BLE name**,
  longest alias first. That rule is implemented and tested in both languages.
- `0x24` is location and radio information, not home time. `0x2F` is the sensor
  setting, not time adjustment. `0x2E` is tide data. `0x3D` is the notification
  manager. `0x47` is watch-initiated and needs no reply.
- `0x1E` stores the timezone as a signed byte in 15-minute units and carries the
  watch's DST **rule index**, so `DST=AUTO` depends on the watch's own rule table.

## Next

1. Run the Android app against the watch: connect, handshake, `Sync time`, and
   compare the watch display. This is the only unverified path left in the app.
2. C session state machine plus a transport interface, so Android and Linux
   share the handshake instead of each reimplementing it in its host language.
3. Add the remaining observed classes (`0x10`, `0x11`, `0x20`, `0x26`, `0x45`,
   `0x3D`) once their field layouts are pinned down, then the convoy channel.

Firmware update is deliberately out of scope: it needs vendor binaries and is
the only path that can brick a watch.

## Provenance and licensing

The spec is derived from a decompiled copy of `com.casio.gshockmove` 3.6.0 and
an HCI capture of the user's own watch. No Casio binary, firmware image, city
table or tide-harmonic asset is redistributed here. The model and flag names are
factual identifiers needed for interoperability.

## Consuming the core

The library is a normal CMake subdirectory. A parent project (for example
Klend-ario, as `libs/firstparty/wristctl`) does:

```cmake
set(WRISTCTL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(libs/firstparty/wristctl wristctl_build)
target_link_libraries(consumer PRIVATE wristctl_core::wristctl_core)
```

`WRISTCTL_BUILD_GUI` defaults to OFF, so the parent needs no Vulkan, Wayland or
font toolchain to build the codecs. The generated C sources are committed, so
the parent never runs Python.

## License

AGPL-3.0; see `LICENSE`. Framework components and their terms are listed in
`THIRD_PARTY.md`.
