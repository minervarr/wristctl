#pragma once

namespace wristctl::gui {

struct GuiSettings {
    int page = 0;
};

/* Reads <config>/gui.conf through arc::fs's layout. Missing or malformed
 * values fall back to the defaults; this never creates the user's directories
 * beyond the app's own config/state scope. */
GuiSettings load_settings();

void save_settings(const GuiSettings &settings);

} // namespace wristctl::gui
