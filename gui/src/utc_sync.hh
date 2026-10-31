#pragma once

#include <string>

#include "wristctl/session.h"

namespace wristctl::gui {

struct UtcClock {
    wc_dst_watch_state state{};
    wc_dst_setting settings[2]{};
    wc_city_name names[2]{};
};

struct ClockRead {
    wc_dst_watch_state state{};
    wc_dst_setting settings[5]{};
    wc_city_name names[5]{};
    bool settings_valid[5]{};
    bool names_valid[5]{};

    bool has_utc_clock() const;
    bool complete() const;
    UtcClock utc_clock() const;
};

bool read_clock(wc_session *session, ClockRead &clock, bool all_slots,
                std::string *error = nullptr);
bool read_utc_clock(wc_session *session, UtcClock &clock,
                    std::string *error = nullptr);
void configure_utc_clock(UtcClock &clock);
bool utc_clock_is_configured(const UtcClock &clock);

} // namespace wristctl::gui
