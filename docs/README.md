# Screenshots

All images come from a demo/recorded transport, never from a watch.

| File | Source |
|---|---|
| `gui-watch.png` | Earlier C++ GUI capture from the recorded GBX-100 demo session, 2026-10-28. It predates the adaptive companion shell. |
| `watch.png`, `clock.png`, `alarms.png`, `timer.png` | GTK prototype (`make ui-demo`), recorded session. |

For new C++ captures, run `./build/linux_debug/gui/wristctl_gui --demo`, use
Diagnostics to choose demo-only scenarios, and capture the window at 960x720,
1440x900, and a narrow touch-like size. `WindowOptions` starts at 960x720;
rendering uses `assets/fonts/ui/ui.otf` through `RasterFont`. On Wayland,
`grim` can capture and crop the app surface. Keep temporary captures outside
the repository and commit only deliberately selected documentation images.
The demo snapshot is not live watch data.
