# wristctl for Android

A single-screen app that drives the C core over JNI. It is deliberately plain:
one `Activity`, no AndroidX, no fragments, no architecture layer. The point is a
working byte path to the watch, not UI.

```
app/src/main/cpp/wristctl_jni.c   JNI shim: codecs + model registry only
app/src/main/cpp/CMakeLists.txt   links the wristctl_core target into libwristctl.so
app/src/main/java/dev/wristctl/Native.java        the whole Java<->C surface
app/src/main/java/dev/wristctl/WatchLink.java     the only Android BLE code
app/src/main/java/dev/wristctl/MainActivity.java  ui + permissions
```

Nothing under `app/src/main/cpp` is a copy of the core: CMake adds
`../../../../../libs/firstparty/wristctl_core` as a subdirectory and links the
`wristctl_core` target, so `make test` in the parent directory tests the exact
same objects the app ships.

## Build and install

Requires JDK 17+, Android SDK 36, NDK 29.0.14206865. `local.properties` points at
the SDK; adjust it for another machine.

```sh
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n dev.wristctl/.MainActivity
```

`./gradlew :app:lintDebug` reports warnings only.

## What the app does

On start it runs a self-test through JNI (encode a `0x09` frame, decode it, look
up `CASIO GBX-100`) and prints the result to the console field and to logcat
under the `wristctl` tag. That catches a broken native build without a watch.

**Connect** scans for a name matching the Casio prefixes, connects, enables
notifications on `26eb0030`, and replays the recorded handshake: `0x23`, `0x10`,
app info write, `0x11`, `0x3B`, `0x3A`, `0x26`, `0x28`, `0x20`, then answers the
watch's `0x47 02` with a bond or a Link Loss Alert write and waits for `0x47 01`.
Every frame is decoded by the C core and appended to the console.

**Sync time** encodes the phone's local time as `0x09` and writes it to
`26eb002d`. It does not touch timezone, DST, or city settings — the same
limitation as `gshockctl time --set`.

The watch advertises for a few seconds after a button press, so Connect keeps
scanning for 30 s. The GBX-100 also regenerates its random address on power-up;
the app always scans by name and only prefers a bonded device as a shortcut.

## Verified

- `./gradlew :app:assembleDebug` builds four ABIs; `libwristctl.so` exports all
  seven `Java_dev_wristctl_Native_*` symbols and links only libc/libm/libdl.
- Installed and launched on a moto g06 (Android 15): process stays up, logcat
  shows `core: wristctl core: 13 class codecs, 137 models, 107 flags`,
  `model lookup: GBX_100`, and
  `self-test: current_time 2026-10-27 18:52:00 weekday=2 nano=0`.
- `lintDebug`: 0 errors, 11 cosmetic warnings (icon, button style, i18n of log
  strings).

Not verified: the BLE path on hardware. The phone was locked (`deviceLocked=1`)
so the buttons could not be pressed, and the watch was not advertising.

## Moving to a native activity

The Java layer is three files and holds no protocol knowledge:

- `Native.java` — keep as is; a native activity can call the same JNI surface.
- `WatchLink.java` — this is the part to replace. It owns scanning, GATT,
  permissions, and the handshake order; all of it is Android API, no codecs.
- `MainActivity.java` — throw away. The `0x47`/`0x28`/`0x23` handling in
  `WatchLink.onFrame` is the only state machine worth porting.

The C side has no Android dependencies at all: `c/` builds unchanged for the host
tests, the JNI library, and any future native transport.
