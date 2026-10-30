#undef NDEBUG

#include <assert.h>

#include <string_view>

#include "demo_scenarios.hh"

int main()
{
    using namespace wristctl::gui;

    SessionSnapshot base;
    base.connected = true;
    base.busy = false;
    base.demo = true;
    base.status = "Recorded data";
    base.error = "recorded warning";
    base.has_condition = true;
    base.battery = 82;
    base.temperature = 24;
    base.mtu = 185;
    base.has_settings = true;
    base.has_alarms = true;
    base.has_timer = true;
    base.has_clock = true;
    base.supports_scan = true;
    base.devices.push_back({"GBX-100", "00:11:22:33:44:55", -52, true});

    const SessionSnapshot recorded = make_demo_snapshot(base, DemoScenario::Recorded);
    assert(recorded.connected == base.connected);
    assert(recorded.status == base.status);
    assert(recorded.error == base.error);
    assert(recorded.battery == base.battery);
    assert(recorded.has_alarms == base.has_alarms);
    assert(recorded.devices.size() == base.devices.size());

    const SessionSnapshot disconnected =
        make_demo_snapshot(base, DemoScenario::Disconnected);
    assert(!disconnected.connected);
    assert(!disconnected.has_condition);
    assert(!disconnected.has_settings);
    assert(!disconnected.has_alarms);
    assert(!disconnected.has_timer);
    assert(!disconnected.has_clock);
    assert(!disconnected.busy);
    assert(disconnected.battery == 0);
    assert(disconnected.temperature == 0);
    assert(disconnected.mtu == 0);

    const SessionSnapshot scanning = make_demo_snapshot(base, DemoScenario::Scanning);
    assert(!scanning.connected);
    assert(scanning.scanning);
    assert(scanning.busy);

    const SessionSnapshot permission =
        make_demo_snapshot(base, DemoScenario::Permission);
    assert(permission.permission_missing);
    assert(permission.supports_scan);

    const SessionSnapshot empty = make_demo_snapshot(base, DemoScenario::Empty);
    assert(!empty.connected);
    assert(!empty.has_alarms);
    assert(!empty.has_timer);
    assert(!empty.has_settings);
    assert(!empty.has_clock);

    assert(make_demo_snapshot(base, DemoScenario::Busy).busy);
    const SessionSnapshot success = make_demo_snapshot(base, DemoScenario::Success);
    assert(success.error.empty());
    assert(success.status == "Completed successfully");
    const SessionSnapshot error = make_demo_snapshot(base, DemoScenario::Error);
    assert(!error.error.empty());
    assert(error.status == "Action failed");

    constexpr DemoScenario scenarios[] = {
        DemoScenario::Recorded, DemoScenario::Disconnected,
        DemoScenario::Scanning, DemoScenario::Permission, DemoScenario::Empty,
        DemoScenario::Dirty, DemoScenario::Busy, DemoScenario::Confirmation,
        DemoScenario::Success, DemoScenario::Error,
    };
    constexpr std::string_view names[] = {
        "Recorded", "Disconnected", "Scanning", "Permission", "Empty",
        "Dirty", "Busy", "Confirmation", "Success", "Error",
    };
    for (int index = 0; index < 10; ++index) {
        assert(!demo_scenario_name(scenarios[index]).empty());
        assert(demo_scenario_name(scenarios[index]) == names[index]);
    }
    return 0;
}
