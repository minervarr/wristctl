# wristctl for Android

The phone frontend is the same application as the desktop: `android_main()`
constructs `WatchView` from `gui/`, `AndroidHost` from App_shell owns the
window, and Vk_Canvas draws every pixel. The manifest names the stock
`android.app.NativeActivity`; there is no Java UI and no XML layout.

The app is offline-first: no Casio account, cloud synchronization, analytics,
telemetry, Play Store integration or online coaching. Optional SNTP UDP time
queries are its only intentional network access.

```
android/app/src/main/cpp/main.cc        android_main, AndroidHost injection
android/app/src/main/cpp/android_link.cc JNI transport and notification queue
android/app/src/main/java/dev/wristctl/BleLink.java  GATT, permissions, finder sound
android/app/src/main/java/dev/wristctl/WatchNotificationListener.java  opt-in listener
android/CMakeLists.txt                  libwristctl.so + framework + assets
```

`BleLink.java` is loaded from native code through the activity's classloader.
Its static block loads `libwristctl`, because `NativeActivity` `dlopen()`s the
library without registering it with the JVM. The class contains no handshake
and no frame decoding: `wc_session` in the C core owns both. The notification
listener is a separate Android system service and uses the same native library.

## Build and install

JDK 17+, Android SDK 36, NDK 29.0.14206865. `local.properties` points at the
SDK on this machine.

```sh
./gradlew :app:assembleDebug
adb -s 192.168.101.6:44427 install -r app/build/outputs/apk/debug/app-debug.apk
adb -s 192.168.101.6:44427 shell am start -n dev.wristctl/android.app.NativeActivity
```

The native library is built from the same `wristctl_core`,
`gui/src/watch_view.cc`, `gui/src/ble_worker.cc`, `gui/src/settings.cc` and
`gui/src/sntp_client.cc` the desktop uses. `android/CMakeLists.txt` adds the
core as a subdirectory; the core is never copied.

## Pairing and connecting

1. Tap **Scan**. On first use Android asks for **Nearby devices**
   (`BLUETOOTH_SCAN` + `BLUETOOTH_CONNECT`; location on Android 11 and older).
   The scan starts automatically after the grant.
2. Press a button on the watch to wake it if it is not in the list.
3. Tap a watch row to select it, then **Pair & connect** (or **Connect** when it
   is already paired).
4. The session connects, writes the current time, and waits for the watch's
   `0x47 02`. `BleLink` calls `createBond()`; confirm the system dialog. If the
   link drops during bonding, the Java transport reconnects and re-subscribes so
   the session can still receive `0x47 01`.
5. **Grant** / **Open settings** and **Turn on** are shown inline when the
   permission or radio is missing. **Show all devices** lists non-Casio names
   too.

**Auto-connect** is on by default and persisted in `gui.conf`. On launch the
worker scans by watch name and reconnects to a paired watch; there is no need
to open the pair screen. The toggle on the Watch page turns it off (and
`Disconnect` also leaves it off for the next launch). When it is off, the pair
screen starts with **Tap Scan to find and pair your watch**.

The desktop application keeps its own auto-connect behaviour and does not
expose the Android device list; `platform_supports_scan()` is false there.

## Shared control pages

Ten pages cover Home, Watch, Clock, Alarms, Timer, Training, Activity, Phone,
Settings and Console. Editors cover five alarms, the six-entry program timer,
five city/DST entries, basic settings, the watch name and app notifications.
The Clock page selects device-local or SNTP UTC synchronization, edits the
ordered server list, optionally syncs on connect, and exposes app and watch
12H/24H formats. SNTP UTC and sync-on-connect default to on, using the same Arch
NTP pool as the Linux host. SNTP runs in the shared native worker, never in
Java. It changes the watch clock, not Android's system clock; demo mode never
accesses the network.

A manual SNTP sync reuses the Home/World state read on connection, preserves
both city IDs and unrelated fields, provisions UTC to both through
`0x1d`/`0x1e`/`0x1f`, then writes corrected UTC with `0x09`. The GBX-100 returns
only slots 0 and 1, so its refresh requests only those slots; other models keep
the five-slot refresh. Sync uses a cached Home/World result even when another
model's five-city refresh is incomplete and does not start another DST read.
Network failure aborts before a watch write.
Sync on connect performs the same setup after the handshake; SNTP failure skips
the connection and retries with a visible error instead of writing Android's
clock.

Alarm, timer and settings writes read back the resulting state where the watch
supports it. Applying the program timer requires a second tap because it
replaces every entry and the repeat count. Notifications and `0x09` have no
readback and must be checked on the watch display.

### Phone notifications and finder

Notification forwarding is off by default. Grant Android's special
**Notification access** access from the Phone page, then enable forwarding. The
listener sends package, title and text only to the app's private native queue;
the shared worker filters them locally by an exact package allowlist and
optional quiet-hour presets before using the existing watch connection. An
empty allowlist permits all packages. No notification content is uploaded.
Fields longer than the protocol limit are filtered rather than truncated, and
the bounded queue drops its oldest entry on overflow.

The watch's Immediate Alert high level plays the phone's local alarm ringtone
for at most 60 seconds. Alert zero, disconnecting, or **Stop phone finder sound**
stops it. These integrations and their watch delivery have not yet been
exercised on a physical phone/watch pair.

Activity/convoy history download, run-measurement settings and GPS/radio-location
upload remain unavailable because their complete wire behavior is not proven.
The app does not invent activity data or send speculative protocol writes.

These write paths are covered by core packet tests and the recorded demo UI.
Only the older local-time write listed below has been verified on the phone and
GBX-100; SNTP, UTC provisioning and the other new writes still require separate
hardware validation.

## Permissions

```
BLUETOOTH_SCAN      (neverForLocation)
BLUETOOTH_CONNECT
BLUETOOTH           maxSdk 30
BLUETOOTH_ADMIN     maxSdk 30
ACCESS_FINE_LOCATION maxSdk 30
INTERNET            (optional SNTP UDP sync)
```

No storage, POST_NOTIFICATIONS or foreground-service permission is requested.
Notification access is a separate, user-controlled Android special access, not
a runtime permission. `gui.conf` lives under the app's private
`internalDataPath`.

## Verified on the moto g06 (Android 15)

- NativeActivity starts, renders the touch layout in portrait and landscape,
  and survives background/return.
- Permission dialog, denial banner, **Open settings**, and Bluetooth-off path.
- Scan list, device selection, pair/connect, settings/alarms/timer read, and
  the time write on the GBX-100.
- Auto-connect on launch with a paired watch; the toggle persists and stops
  the launch scan when off.

Known cosmetic issue: the system status bar remains visible because app_shell's
immersive call does not stick on this device. The footer is inset for the
navigation bar, so the watch content and the hint remain readable.
