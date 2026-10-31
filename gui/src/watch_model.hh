#pragma once

#include <cstdio>
#include <string>

#include "wristctl/generated.h"

namespace wristctl::gui {

inline std::string offset_text(int quarters, bool utc)
{
    const int minutes = quarters * 15;
    const int magnitude = minutes < 0 ? -minutes : minutes;
    char text[32];
    std::snprintf(text, sizeof text, "%s%s%02d:%02d", utc ? "UTC" : "",
                  minutes < 0 ? "-" : "+", magnitude / 60, magnitude % 60);
    return text;
}

inline unsigned alarm_enabled_count(const wc_alarm_first &first,
                                    const wc_alarm_rest &rest) noexcept
{
    unsigned count = first.enabled != 0 ? 1U : 0U;
    for (const auto &alarm : rest.alarms) {
        count += alarm.enabled != 0 ? 1U : 0U;
    }
    return count;
}

inline unsigned timer_round_seconds(const wc_program_timer &timer)
{
    unsigned total = 0;
    for (const auto &workout : timer.workouts) {
        total += static_cast<unsigned>(workout.minutes) * 60U + workout.seconds;
    }
    return total;
}

inline std::string duration_text(unsigned seconds)
{
    char text[32];
    if (seconds >= 3600) {
        std::snprintf(text, sizeof text, "%u:%02u:%02u", seconds / 3600,
                      (seconds / 60) % 60, seconds % 60);
    } else {
        std::snprintf(text, sizeof text, "%u:%02u", seconds / 60, seconds % 60);
    }
    return text;
}

} // namespace wristctl::gui
