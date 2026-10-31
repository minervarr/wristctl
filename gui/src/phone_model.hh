#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace wristctl::gui {

struct PhoneConfig {
    bool forward = false;
    std::vector<std::string> allowed_packages;
    int quiet_start_min = -1;
    int quiet_end_min = -1;
};

inline bool phone_notification_allowed(const PhoneConfig &config,
                                       const std::string &package,
                                       int local_minute)
{
    if (!config.forward) {
        return false;
    }
    if (!config.allowed_packages.empty() &&
        std::find(config.allowed_packages.begin(),
                  config.allowed_packages.end(),
                  package) == config.allowed_packages.end()) {
        return false;
    }
    if (config.quiet_start_min < 0 || config.quiet_end_min < 0) {
        return true;
    }
    const int start = config.quiet_start_min;
    const int end = config.quiet_end_min;
    const bool quiet = start == end
                           ? true
                           : (start < end
                                  ? local_minute >= start && local_minute < end
                                  : local_minute >= start || local_minute < end);
    return !quiet;
}

} // namespace wristctl::gui
