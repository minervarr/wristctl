#pragma once

// The worker's view of a platform BLE link.
//
// One instance owns one transport session. It is single-threaded: the worker
// calls connect/write/read/pump/close on its own thread and may block inside
// them. Desktop implements it over wc_bluez; Android implements it over JNI to
// BleLink.java. The protocol is NOT here — wc_session owns every frame.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "wristctl/session.h"

namespace wristctl::gui {

// Same shape as wc_bluez's cancel callback. Declared here so this header does
// not pull in the Linux transport.
using WatchLinkCancel = bool (*)(void *ctx);

struct WatchDevice {
    std::string name;
    std::string address;
    int rssi = 0;
    bool bonded = false;
};

struct PhoneNotification {
    uint8_t type = 0;
    std::string app;
    std::string title;
    std::string text;
};

class WatchLink {
public:
    virtual ~WatchLink() = default;

    virtual void set_cancel(WatchLinkCancel cancel, void *ctx) = 0;

    // Scans, connects, resolves GATT and subscribes notifications. Returns
    // WC_SESSION_OK or a negative code; error() explains a failure.
    virtual int connect(uint32_t scan_timeout_ms) = 0;

    // Valid after a successful connect(). Must outlive the wc_session built
    // from it.
    virtual const wc_transport *transport() const = 0;

    // Processes platform events while idle. Android needs nothing here.
    virtual int pump(uint32_t timeout_ms) = 0;

    virtual bool link_lost() const = 0;
    virtual const char *address() const = 0;
    virtual const char *error() const = 0;

    // Disconnects and releases the platform handle. Safe to call twice.
    virtual void close() = 0;

    // ── Discovery (Android; BlueZ has no list yet) ──────────────────────────
    virtual bool supports_scan() const { return false; }
    virtual bool start_scan(uint32_t scan_timeout_ms)
    {
        (void)scan_timeout_ms;
        return false;
    }
    virtual void stop_scan() {}
    virtual bool scanning() const { return false; }
    virtual std::vector<WatchDevice> devices() const { return {}; }

    // Connect to an address chosen from the scan list. The default falls back
    // to the platform's own scan-by-name path.
    virtual int connect_to(const std::string &address, uint32_t timeout_ms)
    {
        (void)address;
        return connect(timeout_ms);
    }

    // ── Permission / radio state (Android) ──────────────────────────────────
    virtual bool permission_missing() const { return false; }
    virtual bool bluetooth_off() const { return false; }
    virtual bool location_off() const { return false; }
    virtual bool pairing() const { return false; }
    virtual void request_permissions() {}
    virtual void open_settings() {}
    virtual void open_location_settings() {}
    virtual void enable_bluetooth() {}
    virtual void open_notification_settings() {}
    virtual void stop_phone_finder() {}
    virtual bool notification_access() const { return false; }
};

// Does this platform offer a scan list? False on Linux, true on Android.
bool platform_supports_scan();

// Android's NotificationListener posts into this bounded queue. Other
// platforms have no automatic notification source.
bool platform_take_notification(PhoneNotification &out);

// Implemented once per platform. The worker never knows which one it got.
std::unique_ptr<WatchLink> make_watch_link();

} // namespace wristctl::gui
