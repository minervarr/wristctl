#pragma once

#include <string>
#include <vector>

#include "ui_navigation.hh"

namespace wristctl::gui {

struct GuiSettings {
    Route route = Route::Overview;
    bool auto_connect = true;
    bool use_ntp = true;
    bool sync_on_connect = true;
    std::vector<std::string> ntp_servers = {
        "0.arch.pool.ntp.org", "1.arch.pool.ntp.org",
        "2.arch.pool.ntp.org", "3.arch.pool.ntp.org"};
    int ntp_timeout_ms = 2000;
    bool app_time_24h = true;
    bool home_show_connection = true;
    bool home_show_clock = true;
    bool home_show_alarms = true;
    bool home_show_timer = true;
    bool notify_forward = false;
    std::vector<std::string> notify_packages;
    int notify_quiet_start_min = -1;
    int notify_quiet_end_min = -1;
};

/* Reads <config>/gui.conf through arc::fs's layout. Missing or malformed
 * values fall back to the defaults; this never creates the user's directories
 * beyond the app's own config/state scope. */
GuiSettings load_settings();

void save_settings(const GuiSettings &settings);

} // namespace wristctl::gui
