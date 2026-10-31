#include "utc_sync.hh"

#include <cstdio>
#include <cstring>

namespace wristctl::gui {

bool ClockRead::has_utc_clock() const
{
    return settings_valid[0] && settings_valid[1] && names_valid[0] &&
           names_valid[1];
}

bool ClockRead::complete() const
{
    for (int unit = 0; unit < 5; unit++) {
        if (!settings_valid[unit] || !names_valid[unit]) {
            return false;
        }
    }
    return true;
}

UtcClock ClockRead::utc_clock() const
{
    UtcClock clock;
    clock.state = state;
    memcpy(clock.settings, settings, sizeof clock.settings);
    memcpy(clock.names, names, sizeof clock.names);
    return clock;
}

bool read_clock(wc_session *session, ClockRead &clock, bool all_slots,
                std::string *error)
{
    clock = ClockRead{};
    if (error != nullptr) {
        error->clear();
    }
    auto fail = [error](const char *operation, int rc) {
        if (error != nullptr) {
            char buffer[96];
            std::snprintf(buffer, sizeof buffer, "%s failed (rc %d)", operation,
                          rc);
            *error = buffer;
        }
        return false;
    };

    int rc = wc_session_read_dst_watch_state(session, &clock.state);
    if (rc != WC_SESSION_OK) {
        return fail("0x1d read", rc);
    }

    bool all_settings = true;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (!all_slots && clock.settings_valid[0] && clock.settings_valid[1]) {
            break;
        }
        wc_dst_setting value{};
        rc = wc_session_read_dst_setting(session, &value);
        if (rc != WC_SESSION_OK) {
            all_settings = false;
            fail("0x1e read", rc);
            break;
        }
        if (value.unit >= 5) {
            if (error != nullptr) {
                *error = "0x1e returned invalid unit " +
                         std::to_string(value.unit);
            }
            all_settings = false;
            break;
        }
        if (clock.settings_valid[value.unit]) {
            if (error != nullptr) {
                *error = "0x1e repeated unit " + std::to_string(value.unit);
            }
            all_settings = false;
            break;
        }
        clock.settings[value.unit] = value;
        clock.settings_valid[value.unit] = true;
    }

    const int name_count = all_slots && all_settings ? 5 : 2;
    for (int unit = 0; unit < name_count; unit++) {
        if (!clock.settings_valid[unit]) {
            continue;
        }
        rc = wc_session_read_city_name(session, static_cast<uint8_t>(unit),
                                       &clock.names[unit]);
        if (rc != WC_SESSION_OK || clock.names[unit].unit != unit) {
            fail(unit == 0 ? "0x1f Home read" : "0x1f city read", rc);
            break;
        }
        clock.names_valid[unit] = true;
    }
    return all_slots ? clock.complete() : clock.has_utc_clock();
}

bool read_utc_clock(wc_session *session, UtcClock &clock, std::string *error)
{
    ClockRead read;
    const bool ok = read_clock(session, read, false, error);
    if (!ok) {
        if (error != nullptr && error->empty()) {
            *error = "0x1e/0x1f did not return both Home and World";
        }
        return false;
    }
    clock = read.utc_clock();
    return true;
}

void configure_utc_clock(UtcClock &clock)
{
    clock.state.state_home = 0;
    clock.state.dst_on_home = 0;
    clock.state.dst_auto_home = 0;
    clock.state.dst_auto_rep_home = 0;
    clock.state.state_world = 0;
    clock.state.dst_on_world = 0;
    clock.state.dst_auto_world = 0;
    clock.state.dst_auto_rep_world = 0;

    for (int unit = 0; unit < 2; unit++) {
        clock.settings[unit].timezone = 0;
        clock.settings[unit].dst_diff = 0;
        clock.settings[unit].dst_rule = 0;
        memset(clock.names[unit].name, 0, sizeof clock.names[unit].name);
        memcpy(clock.names[unit].name, "UTC", 3);
    }
}

bool utc_clock_is_configured(const UtcClock &clock)
{
    if (clock.state.state_home != 0 || clock.state.dst_on_home != 0 ||
        clock.state.dst_auto_home != 0 || clock.state.dst_auto_rep_home != 0 ||
        clock.state.state_world != 0 || clock.state.dst_on_world != 0 ||
        clock.state.dst_auto_world != 0 || clock.state.dst_auto_rep_world != 0) {
        return false;
    }
    for (int unit = 0; unit < 2; unit++) {
        if (clock.settings[unit].timezone != 0 ||
            clock.settings[unit].dst_diff != 0 ||
            clock.settings[unit].dst_rule != 0 ||
            strncmp(clock.names[unit].name, "UTC",
                    sizeof clock.names[unit].name) != 0) {
            return false;
        }
    }
    return true;
}

} // namespace wristctl::gui
