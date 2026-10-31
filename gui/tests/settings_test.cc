#undef NDEBUG

#include <assert.h>
#include <stdlib.h>

#include <fstream>
#include <string>
#include <vector>

#include "settings.hh"

int main()
{
    setenv("XDG_CONFIG_HOME", "/tmp/opencode/wristctl-settings-test", 1);
    using wristctl::gui::GuiSettings;
    using wristctl::gui::load_settings;
    using wristctl::gui::save_settings;

    save_settings({3, true});
    GuiSettings loaded = load_settings();
    assert(loaded.page == 3);
    assert(loaded.auto_connect);
    assert(loaded.use_ntp);
    assert(loaded.sync_on_connect);
    assert(loaded.ntp_servers.size() == 4);
    assert(loaded.ntp_servers[0] == "0.arch.pool.ntp.org");
    assert(loaded.ntp_timeout_ms == 2000);

    GuiSettings custom{5, false};
    custom.use_ntp = true;
    custom.sync_on_connect = true;
    custom.ntp_servers = {"ntp.local", "192.0.2.10", "2001:db8::1"};
    custom.ntp_timeout_ms = 750;
    custom.app_time_24h = false;
    custom.home_show_connection = false;
    custom.home_show_clock = false;
    custom.home_show_alarms = false;
    custom.home_show_timer = false;
    custom.notify_forward = true;
    custom.notify_packages = {"com.example.mail", "org.example.chat"};
    custom.notify_quiet_start_min = 22 * 60;
    custom.notify_quiet_end_min = 7 * 60;
    save_settings(custom);
    loaded = load_settings();
    assert(loaded.page == 5);
    assert(!loaded.auto_connect);
    assert(loaded.use_ntp);
    assert(loaded.sync_on_connect);
    assert(loaded.ntp_servers == custom.ntp_servers);
    assert(loaded.ntp_timeout_ms == 750);
    assert(!loaded.app_time_24h);
    assert(!loaded.home_show_connection);
    assert(!loaded.home_show_clock);
    assert(!loaded.home_show_alarms);
    assert(!loaded.home_show_timer);
    assert(loaded.notify_forward);
    assert(loaded.notify_packages == custom.notify_packages);
    assert(loaded.notify_quiet_start_min == 22 * 60);
    assert(loaded.notify_quiet_end_min == 7 * 60);

    {
        std::ofstream legacy("/tmp/opencode/wristctl-settings-test/wristctl/gui.conf",
                             std::ios::trunc);
        legacy << "page=1\nauto_connect=1\n";
    }
    loaded = load_settings();
    assert(loaded.page == 1);
    assert(loaded.auto_connect);
    assert(loaded.use_ntp);
    assert(loaded.sync_on_connect);
    assert(loaded.ntp_servers.size() == 4);

    {
        std::ofstream malformed("/tmp/opencode/wristctl-settings-test/wristctl/gui.conf",
                                std::ios::trunc);
        malformed << "page=99\nauto_connect=yes\nuse_ntp=2\n"
                     "sync_on_connect=no\nntp_server=bad host\n"
                     "ntp_timeout_ms=1\nnotify_forward=maybe\n"
                     "notify_package=bad package\n"
                     "notify_quiet_start_min=2000\nnotify_quiet_end_min=-4\n";
    }
    loaded = load_settings();
    assert(loaded.page == 0);
    assert(loaded.auto_connect);
    assert(loaded.use_ntp);
    assert(loaded.sync_on_connect);
    assert(loaded.ntp_servers.size() == 4);
    assert(loaded.ntp_timeout_ms == 2000);
    assert(!loaded.notify_forward);
    assert(loaded.notify_packages.empty());
    assert(loaded.notify_quiet_start_min == -1);
    assert(loaded.notify_quiet_end_min == -1);
    return 0;
}
