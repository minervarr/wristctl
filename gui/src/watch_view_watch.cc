#include "watch_view.hh"

#include <algorithm>
#include <cctype>

#include "ui_components.hh"
#include "ui_theme.hh"

namespace wristctl::gui {
namespace {

bool looks_like_watch_name(const std::string &name)
{
    static const char *const prefixes[] = {
        "CASIO", "GBX", "GBD", "GPR", "GWF", "GST", "GMW", "MRG", "DW-", "GM-"};
    std::string upper;
    upper.reserve(name.size());
    for (char c : name) {
        upper.push_back(static_cast<char>(
            std::toupper(static_cast<unsigned char>(c))));
    }
    for (const char *prefix : prefixes) {
        if (upper.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

float WatchView::drawWatchPage(Canvas &canvas, const Layout &layout)
{
    if (!snapshot_.connected) {
        return drawPairPage(canvas, layout);
    }

    const auto &palette = dark_palette();
    const float scroll = page_scroll(route_index(route_));
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = touch_ ? 12.0f * d : 16.0f;
    float cursor = top;

    canvas.text("Watch", x, cursor - scroll, layout.page_title_size,
                palette.text_primary);
    cursor += layout.page_title_size + 8.0f * d;
    canvas.text("Connection and condition, without the protocol noise.", x,
                cursor - scroll, layout.small_size, palette.text_secondary);
    cursor += layout.small_size + 20.0f * d;

    const Rect identity = {x, cursor - scroll, layout.content_w, 142.0f * d};
    draw_card(canvas, identity, 12.0f * d);
    const float pad = 18.0f * d;
    const std::string name = snapshot_.name.empty() ? "CASIO watch" : snapshot_.name;
    canvas.text(truncateToWidth(canvas, name, identity.w - 2.0f * pad,
                                layout.page_title_size),
                identity.x + pad, identity.y + pad, layout.page_title_size,
                palette.text_primary);
    draw_badge(canvas, "Connected", identity.x + pad,
               identity.y + pad + 38.0f * d, layout.small_size, palette.success);
    const std::string detail = snapshot_.model.empty()
        ? snapshot_.address
        : snapshot_.model + (snapshot_.address.empty() ? "" : " · " + snapshot_.address);
    canvas.text(truncateToWidth(canvas, detail, identity.w - 2.0f * pad,
                                layout.small_size),
                identity.x + pad, identity.y + identity.h - pad - layout.small_size,
                layout.small_size, palette.text_secondary);
    cursor += identity.h + gap;

    const Rect condition = {x, cursor - scroll, layout.content_w, 108.0f * d};
    draw_card(canvas, condition, 12.0f * d);
    canvas.text("Condition", condition.x + pad, condition.y + pad,
                layout.small_size, palette.text_secondary);
    if (snapshot_.has_condition) {
        const std::string values = std::to_string(snapshot_.battery) + "% battery   ·   " +
                                   std::to_string(snapshot_.temperature) + " °C";
        canvas.text(truncateToWidth(canvas, values, condition.w - 2.0f * pad,
                                    layout.date_size),
                    condition.x + pad, condition.y + pad + 30.0f * d,
                    layout.date_size, palette.text_primary);
    } else {
        canvas.text("Refresh to read battery and temperature", condition.x + pad,
                    condition.y + pad + 34.0f * d, layout.text_size,
                    palette.text_secondary);
    }
    cursor += condition.h + gap;

    auto button = [&](const Rect &rect, const std::string &label, Action action,
                      ButtonVariant variant = ButtonVariant::Secondary) {
        draw_button(canvas, rect, label, variant, layout.small_size);
        hits_.push_back({rect, action, 0, {}, 0, 0});
    };
    const float half = (layout.content_w - gap) * 0.5f;
    button({x, cursor - scroll, half, layout.action_h}, "Refresh",
           Action::Refresh, ButtonVariant::Primary);
    button({x + half + gap, cursor - scroll, half, layout.action_h}, "Disconnect",
           Action::Disconnect, ButtonVariant::Quiet);
    cursor += layout.action_h + gap;
    button({x, cursor - scroll, layout.content_w, layout.action_h},
           snapshot_.auto_connect ? "Auto-connect on" : "Auto-connect off",
           Action::ToggleAutoConnect);
    cursor += layout.action_h + gap;
    cursor = drawNote(canvas, layout,
                      "Auto-connect looks for your paired watch when the app opens.",
                      x, cursor - scroll, palette.text_secondary) +
             scroll;
    return cursor - top;
}

float WatchView::drawPairPage(Canvas &canvas, const Layout &layout)
{
    const auto &palette = dark_palette();
    const float scroll = page_scroll(route_index(route_));
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float w = layout.content_w;
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = touch_ ? 10.0f * d : 12.0f;
    const float row_h = touch_ ? 72.0f * d : 64.0f;
    float cursor = top;

    canvas.text("Find your watch", x, cursor - scroll, layout.page_title_size,
                palette.text_primary);
    cursor += layout.page_title_size + 8.0f * d;
    canvas.text("Wake the watch, scan, then select it to pair or connect.", x,
                cursor - scroll, layout.small_size, palette.text_secondary);
    cursor += layout.small_size + 18.0f * d;

    auto button = [&](const Rect &rect, const std::string &label, Action action,
                      ButtonVariant variant = ButtonVariant::Secondary,
                      bool enabled = true) {
        draw_button(canvas, rect, label, variant, layout.small_size, enabled);
        if (enabled) {
            hits_.push_back({rect, action, 0, {}, 0, 0});
        }
    };
    const bool blocked = snapshot_.permission_missing || snapshot_.bluetooth_off ||
                         snapshot_.location_off;
    if (snapshot_.permission_missing) {
        draw_banner(canvas, {x, cursor - scroll, w, 44.0f * d},
                    "Nearby devices permission is required", layout.small_size,
                    palette.warning);
        cursor += 52.0f * d;
        const float half = (w - gap) * 0.5f;
        button({x, cursor - scroll, half, layout.action_h}, "Grant permission",
               Action::Grant, ButtonVariant::Primary);
        button({x + half + gap, cursor - scroll, half, layout.action_h},
               "Open settings", Action::OpenSettings);
        cursor += layout.action_h + gap;
    }
    if (snapshot_.bluetooth_off) {
        draw_banner(canvas, {x, cursor - scroll, w, 44.0f * d}, "Bluetooth is off",
                    layout.small_size, palette.warning);
        cursor += 52.0f * d;
        button({x, cursor - scroll, w, layout.action_h}, "Turn on Bluetooth",
               Action::TurnOnBluetooth, ButtonVariant::Primary);
        cursor += layout.action_h + gap;
    }
    if (snapshot_.location_off) {
        draw_banner(canvas, {x, cursor - scroll, w, 44.0f * d},
                    "Location services are needed for scanning on this device",
                    layout.small_size, palette.warning);
        cursor += 52.0f * d;
        button({x, cursor - scroll, w, layout.action_h}, "Open location settings",
               Action::OpenLocation, ButtonVariant::Primary);
        cursor += layout.action_h + gap;
    }
    if (!snapshot_.error.empty() && !blocked) {
        draw_banner(canvas, {x, cursor - scroll, w, 44.0f * d},
                    truncateToWidth(canvas, snapshot_.error, w - 28.0f * d,
                                    layout.small_size),
                    layout.small_size, palette.error);
        cursor += 52.0f * d;
    }

    if (!blocked) {
        const float half = (w - gap) * 0.5f;
        button({x, cursor - scroll, half, layout.action_h},
               snapshot_.scanning ? "Stop scan" : "Scan",
               snapshot_.scanning ? Action::StopScan : Action::Scan,
               ButtonVariant::Secondary);
        button({x + half + gap, cursor - scroll, half, layout.action_h},
               selected_is_bonded() ? "Connect" : "Pair & connect",
               Action::ConnectSelected, ButtonVariant::Primary,
               !selected_address_.empty());
        cursor += layout.action_h + gap;
    }

    if (snapshot_.scanning) {
        cursor = drawNote(canvas, layout,
                          "Scanning now. Press a watch button if it does not appear.",
                          x, cursor - scroll, palette.text_secondary) +
                 scroll + gap;
    }

    std::vector<const WatchDevice *> visible;
    for (const WatchDevice &device : snapshot_.devices) {
        if (show_all_ || looks_like_watch_name(device.name)) {
            visible.push_back(&device);
        }
    }
    std::stable_sort(visible.begin(), visible.end(),
                     [](const WatchDevice *a, const WatchDevice *b) {
                         if (a->bonded != b->bonded) {
                             return a->bonded;
                         }
                         return a->name < b->name;
                     });

    if (visible.empty()) {
        const Rect empty = {x, cursor - scroll, w, 132.0f * d};
        draw_card(canvas, empty, 12.0f * d);
        draw_empty_state(canvas, empty, blocked ? "Scanning is unavailable" : "No watches yet",
                         blocked ? "Resolve the item above to continue."
                                 : "Start a scan with your watch awake.",
                         layout.date_size, layout.small_size);
        cursor += empty.h + gap;
    } else {
        canvas.text("Nearby watches", x, cursor - scroll, layout.date_size,
                    palette.text_primary);
        cursor += layout.date_size + 12.0f * d;
        for (const WatchDevice *device : visible) {
            const Rect row = {x, cursor - scroll, w, row_h};
            draw_card(canvas, row, 12.0f * d);
            if (device->address == selected_address_) {
                canvas.rect(row.x, row.y, 4.0f * d, row.h, palette.accent, 2.0f * d);
            }
            const float pad = 16.0f * d;
            canvas.text(truncateToWidth(canvas, device->name, w - 130.0f * d,
                                        layout.text_size),
                        row.x + pad, row.y + 12.0f * d, layout.text_size,
                        palette.text_primary);
            std::string detail = device->address;
            if (device->rssi != 0) {
                detail += " · " + std::to_string(device->rssi) + " dBm";
            }
            canvas.text(truncateToWidth(canvas, detail, w - 2.0f * pad,
                                        layout.small_size),
                        row.x + pad, row.y + 38.0f * d, layout.small_size,
                        palette.text_secondary);
            draw_badge(canvas, device->bonded ? "Paired" : "New",
                       row.x + row.w - 86.0f * d, row.y + 12.0f * d,
                       layout.small_size,
                       device->bonded ? palette.success : palette.text_secondary);
            addHit(row, Action::SelectDevice, device->address);
            cursor += row_h + gap;
        }
    }

    if (!snapshot_.devices.empty()) {
        button({x, cursor - scroll, w, layout.action_h},
               show_all_ ? "Show Casio only" : "Show all devices", Action::ToggleAll,
               ButtonVariant::Quiet);
        cursor += layout.action_h + gap;
    }
    button({x, cursor - scroll, w, layout.action_h},
           snapshot_.auto_connect ? "Auto-connect on" : "Auto-connect off",
           Action::ToggleAutoConnect);
    cursor += layout.action_h + gap;
    return cursor - top;
}

} // namespace wristctl::gui
