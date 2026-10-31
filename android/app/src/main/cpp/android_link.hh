#pragma once

// The one Android-specific setup step the entry point owes the BLE bridge: the
// JavaVM and the Activity object used to load dev.wristctl.BleLink through the
// app's classloader. Call before WatchView starts the worker.
struct android_app;

namespace wristctl::gui {

void android_link_set_app(android_app *app);

} // namespace wristctl::gui
