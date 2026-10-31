#include "settings.hh"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>

#include "arc/fs/paths.hh"

namespace wristctl::gui {
namespace {

constexpr const char *kAppName = "wristctl";
constexpr const char *kFileName = "gui.conf";

std::string config_path()
{
    const arc::fs::Layout layout = arc::fs::layout(kAppName);
    arc::fs::ensureLayout(layout, arc::fs::LayoutScope::App);
    return arc::fs::join(layout.config, kFileName);
}

bool valid_server(const std::string &server)
{
    if (server.empty() || server.size() > 253) {
        return false;
    }
    for (const unsigned char c : server) {
        if (!std::isalnum(c) && c != '.' && c != '-' && c != ':') {
            return false;
        }
    }
    return true;
}

bool parse_bool(const std::string &value, bool &out)
{
    if (value != "0" && value != "1") {
        return false;
    }
    out = value == "1";
    return true;
}

bool valid_package(const std::string &package)
{
    if (package.empty() || package.size() > 255) {
        return false;
    }
    for (const unsigned char c : package) {
        if (!std::isalnum(c) && c != '.' && c != '_') {
            return false;
        }
    }
    return true;
}

bool valid_quiet_minutes(const std::string &value, int &out)
{
    const int minutes = std::atoi(value.c_str());
    if (minutes < -1 || minutes >= 24 * 60) {
        return false;
    }
    out = minutes;
    return true;
}

} // namespace

GuiSettings load_settings()
{
    GuiSettings settings;
    bool have_server = false;
    std::ifstream input(config_path());
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "page") {
            const int page = std::atoi(value.c_str());
            if (page >= 0 && page < 10) {
                settings.page = page;
            }
        } else if (key == "auto_connect") {
            parse_bool(value, settings.auto_connect);
        } else if (key == "use_ntp") {
            parse_bool(value, settings.use_ntp);
        } else if (key == "sync_on_connect") {
            parse_bool(value, settings.sync_on_connect);
        } else if (key == "ntp_server" && valid_server(value)) {
            if (!have_server) {
                settings.ntp_servers.clear();
                have_server = true;
            }
            settings.ntp_servers.push_back(value);
        } else if (key == "ntp_timeout_ms") {
            const int timeout = std::atoi(value.c_str());
            if (timeout >= 100 && timeout <= 30000) {
                settings.ntp_timeout_ms = timeout;
            }
        } else if (key == "app_time_24h") {
            parse_bool(value, settings.app_time_24h);
        } else if (key == "home_show_connection") {
            parse_bool(value, settings.home_show_connection);
        } else if (key == "home_show_clock") {
            parse_bool(value, settings.home_show_clock);
        } else if (key == "home_show_alarms") {
            parse_bool(value, settings.home_show_alarms);
        } else if (key == "home_show_timer") {
            parse_bool(value, settings.home_show_timer);
        } else if (key == "notify_forward") {
            parse_bool(value, settings.notify_forward);
        } else if (key == "notify_package" && valid_package(value)) {
            settings.notify_packages.push_back(value);
        } else if (key == "notify_quiet_start_min") {
            valid_quiet_minutes(value, settings.notify_quiet_start_min);
        } else if (key == "notify_quiet_end_min") {
            valid_quiet_minutes(value, settings.notify_quiet_end_min);
        }
    }
    return settings;
}

void save_settings(const GuiSettings &settings)
{
    std::ofstream output(config_path(), std::ios::trunc);
    if (!output) {
        return;
    }
    output << "page=" << settings.page << "\n";
    output << "auto_connect=" << (settings.auto_connect ? 1 : 0) << "\n";
    output << "use_ntp=" << (settings.use_ntp ? 1 : 0) << "\n";
    output << "sync_on_connect=" << (settings.sync_on_connect ? 1 : 0) << "\n";
    for (const std::string &server : settings.ntp_servers) {
        output << "ntp_server=" << server << "\n";
    }
    output << "ntp_timeout_ms=" << settings.ntp_timeout_ms << "\n";
    output << "app_time_24h=" << (settings.app_time_24h ? 1 : 0) << "\n";
    output << "home_show_connection="
           << (settings.home_show_connection ? 1 : 0) << "\n";
    output << "home_show_clock=" << (settings.home_show_clock ? 1 : 0) << "\n";
    output << "home_show_alarms=" << (settings.home_show_alarms ? 1 : 0) << "\n";
    output << "home_show_timer=" << (settings.home_show_timer ? 1 : 0) << "\n";
    output << "notify_forward=" << (settings.notify_forward ? 1 : 0) << "\n";
    for (const std::string &package : settings.notify_packages) {
        output << "notify_package=" << package << "\n";
    }
    output << "notify_quiet_start_min=" << settings.notify_quiet_start_min << "\n";
    output << "notify_quiet_end_min=" << settings.notify_quiet_end_min << "\n";
}

} // namespace wristctl::gui
