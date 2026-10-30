#undef NDEBUG

#include <assert.h>

#include <string_view>

#include "ui_navigation.hh"

int main()
{
    using namespace wristctl::gui;

    constexpr Route legacy_routes[] = {
        Route::Overview, Route::Watch, Route::Time, Route::Alarms,
        Route::Timer, Route::Timer, Route::Settings, Route::Phone,
        Route::Settings, Route::Diagnostics,
    };
    for (int page = 0; page < 10; ++page) {
        assert(route_from_legacy_page(page) == legacy_routes[page]);
    }
    assert(route_from_legacy_page(-1) == Route::Overview);
    assert(route_from_legacy_page(10) == Route::Overview);

    constexpr std::string_view names[] = {
        "Overview", "Watch", "Time", "Alarms",
        "Timer", "Phone", "Settings", "Diagnostics",
    };
    constexpr Route routes[] = {
        Route::Overview, Route::Watch, Route::Time, Route::Alarms,
        Route::Timer, Route::Phone, Route::Settings, Route::Diagnostics,
    };
    for (int index = 0; index < 8; ++index) {
        assert(route_index(routes[index]) == index);
        assert(route_name(routes[index]) == names[index]);
    }

    assert(primary_destination_index(Route::Overview) == 0);
    assert(primary_destination_index(Route::Time) == 1);
    assert(primary_destination_index(Route::Alarms) == 2);
    assert(primary_destination_index(Route::Timer) == 3);
    assert(primary_destination_index(Route::Phone) == 4);
    assert(primary_destination_index(Route::Watch) == -1);
    assert(primary_destination_index(Route::Settings) == -1);
    assert(primary_destination_index(Route::Diagnostics) == -1);
    return 0;
}
