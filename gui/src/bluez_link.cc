#include "watch_link.hh"

#include "wristctl/bluez.h"

namespace wristctl::gui {
namespace {

// Desktop wrapper: the BlueZ transport already has the blocking semantics the
// worker needs, so this is a rename with an owned handle, not a reimplementation.
class BluezWatchLink final : public WatchLink {
public:
    BluezWatchLink() : bluez_(wc_bluez_new()) {}

    ~BluezWatchLink() override { close(); }

    void set_cancel(WatchLinkCancel cancel, void *ctx) override
    {
        if (bluez_ != nullptr) {
            wc_bluez_set_cancel(bluez_, cancel, ctx);
        }
    }

    int connect(uint32_t scan_timeout_ms) override
    {
        if (bluez_ == nullptr) {
            return WC_SESSION_ERR_TRANSPORT;
        }
        return wc_bluez_connect(bluez_, nullptr, nullptr, scan_timeout_ms);
    }

    const wc_transport *transport() const override
    {
        return bluez_ != nullptr ? wc_bluez_transport(bluez_) : nullptr;
    }

    int pump(uint32_t timeout_ms) override
    {
        return bluez_ != nullptr ? wc_bluez_pump(bluez_, timeout_ms) : -1;
    }

    bool link_lost() const override
    {
        return bluez_ != nullptr && wc_bluez_link_lost(bluez_);
    }

    const char *address() const override
    {
        return bluez_ != nullptr ? wc_bluez_address(bluez_) : "";
    }

    const char *error() const override
    {
        return bluez_ != nullptr ? wc_bluez_error(bluez_) : "BlueZ unavailable";
    }

    void close() override
    {
        if (bluez_ != nullptr) {
            wc_bluez_free(bluez_);
            bluez_ = nullptr;
        }
    }

private:
    wc_bluez *bluez_ = nullptr;
};

} // namespace

bool platform_supports_scan()
{
    // BlueZ's transport scans for one watch and connects; it does not expose a
    // device list to this frontend yet. The Android flow is the pairing UI.
    return false;
}

bool platform_take_notification(PhoneNotification &)
{
    return false;
}

std::unique_ptr<WatchLink> make_watch_link()
{
    return std::make_unique<BluezWatchLink>();
}

} // namespace wristctl::gui
