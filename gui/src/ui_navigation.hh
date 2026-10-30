#pragma once

#include <cstdint>
#include <string_view>

namespace wristctl::gui {

enum class Route : uint8_t {
    Overview,
    Watch,
    Time,
    Alarms,
    Timer,
    Phone,
    Settings,
    Diagnostics,
    Count,
};

Route route_from_legacy_page(int page) noexcept;
int route_index(Route route) noexcept;
int primary_destination_index(Route route) noexcept;
std::string_view route_name(Route route) noexcept;

} // namespace wristctl::gui
