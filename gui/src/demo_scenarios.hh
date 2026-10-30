#pragma once

#include <string_view>

#include "ble_worker.hh"

namespace wristctl::gui {

enum class DemoScenario {
    Recorded,
    Disconnected,
    Scanning,
    Permission,
    Empty,
    Dirty,
    Busy,
    Confirmation,
    Success,
    Error,
};

SessionSnapshot make_demo_snapshot(const SessionSnapshot &base,
                                   DemoScenario scenario);
std::string_view demo_scenario_name(DemoScenario scenario) noexcept;

} // namespace wristctl::gui
