#include "demo_scenarios.hh"

namespace wristctl::gui {
namespace {

void clear_watch_data(SessionSnapshot &snapshot)
{
    snapshot.connected = false;
    snapshot.busy = false;
    snapshot.scanning = false;
    snapshot.has_condition = false;
    snapshot.battery = 0;
    snapshot.temperature = 0;
    snapshot.mtu = 0;
    snapshot.has_settings = false;
    snapshot.has_alarms = false;
    snapshot.has_timer = false;
    snapshot.has_clock = false;
    snapshot.has_utc_clock = false;
    snapshot.has_time_sample = false;
    snapshot.error.clear();
}

} // namespace

SessionSnapshot make_demo_snapshot(const SessionSnapshot &base,
                                   DemoScenario scenario)
{
    SessionSnapshot snapshot = base;
    switch (scenario) {
    case DemoScenario::Recorded:
        return snapshot;
    case DemoScenario::Disconnected:
        clear_watch_data(snapshot);
        snapshot.status = "Disconnected";
        break;
    case DemoScenario::Scanning:
        clear_watch_data(snapshot);
        snapshot.scanning = true;
        snapshot.busy = true;
        snapshot.status = "Scanning for nearby watches";
        break;
    case DemoScenario::Permission:
        clear_watch_data(snapshot);
        snapshot.permission_missing = true;
        snapshot.supports_scan = true;
        snapshot.status = "Bluetooth permission needed";
        break;
    case DemoScenario::Empty:
        clear_watch_data(snapshot);
        snapshot.status = "No watch data available";
        break;
    case DemoScenario::Dirty:
    case DemoScenario::Confirmation:
        break;
    case DemoScenario::Busy:
        snapshot.busy = true;
        snapshot.status = "Working with the watch";
        break;
    case DemoScenario::Success:
        snapshot.busy = false;
        snapshot.error.clear();
        snapshot.status = "Completed successfully";
        break;
    case DemoScenario::Error:
        snapshot.busy = false;
        snapshot.error = "The watch could not complete this action";
        snapshot.status = "Action failed";
        break;
    }
    return snapshot;
}

std::string_view demo_scenario_name(DemoScenario scenario) noexcept
{
    switch (scenario) {
    case DemoScenario::Recorded: return "Recorded";
    case DemoScenario::Disconnected: return "Disconnected";
    case DemoScenario::Scanning: return "Scanning";
    case DemoScenario::Permission: return "Permission";
    case DemoScenario::Empty: return "Empty";
    case DemoScenario::Dirty: return "Dirty";
    case DemoScenario::Busy: return "Busy";
    case DemoScenario::Confirmation: return "Confirmation";
    case DemoScenario::Success: return "Success";
    case DemoScenario::Error: return "Error";
    }
    return "Recorded";
}

} // namespace wristctl::gui
