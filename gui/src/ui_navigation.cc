#include "ui_navigation.hh"

namespace wristctl::gui {

Route route_from_legacy_page(int page) noexcept
{
    switch (page) {
    case 0: return Route::Overview;
    case 1: return Route::Watch;
    case 2: return Route::Time;
    case 3: return Route::Alarms;
    case 4:
    case 5: return Route::Timer;
    case 6:
    case 8: return Route::Settings;
    case 7: return Route::Phone;
    case 9: return Route::Diagnostics;
    default: return Route::Overview;
    }
}

int route_index(Route route) noexcept
{
    const int index = static_cast<int>(route);
    return index >= 0 && index < static_cast<int>(Route::Count) ? index : 0;
}

int primary_destination_index(Route route) noexcept
{
    switch (route) {
    case Route::Overview: return 0;
    case Route::Time: return 1;
    case Route::Alarms: return 2;
    case Route::Timer: return 3;
    case Route::Phone: return 4;
    default: return -1;
    }
}

std::string_view route_name(Route route) noexcept
{
    switch (route) {
    case Route::Overview: return "Overview";
    case Route::Watch: return "Watch";
    case Route::Time: return "Time";
    case Route::Alarms: return "Alarms";
    case Route::Timer: return "Timer";
    case Route::Phone: return "Phone";
    case Route::Settings: return "Settings";
    case Route::Diagnostics: return "Diagnostics";
    default: return "Overview";
    }
}

} // namespace wristctl::gui
