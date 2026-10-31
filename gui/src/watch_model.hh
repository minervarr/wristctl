#pragma once

#include <cstdio>
#include <string>

#include "wristctl/generated.h"

namespace wristctl::gui {

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
