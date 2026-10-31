#include "watch_view.hh"

#include <algorithm>
#include <cstdio>

#include "ui_components.hh"
#include "ui_theme.hh"

namespace wristctl::gui {

float WatchView::drawNotifyPage(Canvas &canvas, const Layout &layout)
{
    const auto &palette = dark_palette();
    const float scroll = page_scroll(route_index(route_));
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float w = layout.content_w;
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = touch_ ? 10.0f * d : 12.0f;
    const float half = (w - gap) * 0.5f;
    float cursor = top;

    auto button = [&](const Rect &rect, const std::string &label, Action action,
                      ButtonVariant variant = ButtonVariant::Secondary,
                      int arg = 0) {
        draw_button(canvas, rect, label, variant, layout.small_size);
        hits_.push_back({rect, action, 0, {}, arg, 0});
    };

    canvas.text("Phone", x, cursor - scroll, layout.page_title_size,
                palette.text_primary);
    cursor += layout.page_title_size + 8.0f * d;
    canvas.text("Notifications and finder, without leaving your watch workflow.",
                x, cursor - scroll, layout.small_size, palette.text_secondary);
    cursor += layout.small_size + 18.0f * d;

    draw_section_title(canvas, "Notification forwarding", x, cursor - scroll,
                       layout.date_size);
    cursor += layout.date_size + 12.0f * d;
    if (snapshot_.supports_scan) {
        cursor = drawKeyValue(canvas, layout, "Notification access",
                              snapshot_.notification_access
                                  ? "Granted"
                                  : "Not granted · connect to refresh",
                              x, cursor - scroll,
                              snapshot_.notification_access ? palette.success
                                                            : palette.warning) +
                 scroll + gap;
        button({x, cursor - scroll, half, layout.action_h},
               settings_.notify_forward ? "Forwarding: on" : "Forwarding: off",
               Action::ToggleNotifyForward,
               settings_.notify_forward ? ButtonVariant::Primary
                                        : ButtonVariant::Secondary);
        button({x + half + gap, cursor - scroll, half, layout.action_h},
               "Notification access", Action::OpenNotificationAccess);
        cursor += layout.action_h + gap;

        button({x, cursor - scroll, half, layout.action_h}, "Allowed apps",
               Action::EditField, ButtonVariant::Secondary, EDIT_NOTIFY_PACKAGES);
        char quiet[48] = "Quiet hours: off";
        if (settings_.notify_quiet_start_min >= 0 &&
            settings_.notify_quiet_end_min >= 0) {
            std::snprintf(quiet, sizeof quiet, "Quiet %02d:%02d–%02d:%02d",
                          settings_.notify_quiet_start_min / 60,
                          settings_.notify_quiet_start_min % 60,
                          settings_.notify_quiet_end_min / 60,
                          settings_.notify_quiet_end_min % 60);
        }
        button({x + half + gap, cursor - scroll, half, layout.action_h}, quiet,
               Action::NotifyQuietHours);
        cursor += layout.action_h + gap;
        cursor = drawNote(canvas, layout,
                          "Allowed packages (blank allows all): " +
                              (notify_packages_text_.empty()
                                   ? std::string("All apps")
                                   : notify_packages_text_),
                          x, cursor - scroll, palette.text_secondary) +
                 scroll + gap;
        char delivery[96];
        std::snprintf(delivery, sizeof delivery, "%u forwarded · %u filtered",
                      snapshot_.notifications_forwarded,
                      snapshot_.notifications_filtered);
        cursor = drawKeyValue(canvas, layout, "Local delivery", delivery, x,
                              cursor - scroll) +
                 scroll + gap;
    } else {
        const Rect unavailable = {x, cursor - scroll, w, 116.0f * d};
        draw_card(canvas, unavailable, 12.0f * d);
        draw_empty_state(canvas, unavailable, "Forwarding needs Android",
                         "Automatic notification access is unavailable here. Manual tests still work.",
                         layout.date_size, layout.small_size);
        cursor += unavailable.h + gap;
    }

    draw_section_title(canvas, "Phone finder", x, cursor - scroll,
                       layout.date_size);
    cursor += layout.date_size + 12.0f * d;
    cursor = drawNote(canvas, layout,
                      "A high alert from the watch plays this phone's alarm for at most 60 seconds. Alert zero or disconnecting stops it.",
                      x, cursor - scroll, palette.text_secondary) +
             scroll + gap;
    button({x, cursor - scroll, w, layout.action_h}, "Stop phone finder sound",
           Action::StopPhoneFinder);
    cursor += layout.action_h + gap * 1.5f;

    draw_section_title(canvas, "Manual notification test", x, cursor - scroll,
                       layout.date_size);
    cursor += layout.date_size + 12.0f * d;
    cursor = drawNote(canvas, layout,
                      "Compose a one-off notification and confirm it on the watch.",
                      x, cursor - scroll, palette.text_secondary) +
             scroll + gap;
    button({x, cursor - scroll, w, layout.action_h}, "Open notification composer",
           Action::NotificationTest, ButtonVariant::Primary);
    cursor += layout.action_h + gap;
    return cursor - top;
}

float WatchView::drawNotificationTest(Canvas &canvas, const Layout &layout)
{
    const auto &palette = dark_palette();
    const float scroll = page_scroll(route_index(route_));
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float w = layout.content_w;
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = touch_ ? 10.0f * d : 12.0f;
    const float half = (w - gap) * 0.5f;
    float cursor = top;

    auto button = [&](const Rect &rect, const std::string &label, Action action,
                      ButtonVariant variant = ButtonVariant::Secondary,
                      int arg = 0) {
        draw_button(canvas, rect, label, variant, layout.small_size);
        hits_.push_back({rect, action, 0, {}, arg, 0});
    };

    button({x, cursor - scroll, 96.0f * d, layout.action_h}, "Back",
           Action::CloseDetail, ButtonVariant::Quiet);
    canvas.text("Notification test", x + 112.0f * d,
                cursor - scroll +
                    (layout.action_h - layout.page_title_size) * 0.5f,
                layout.page_title_size, palette.text_primary);
    cursor += layout.action_h + gap * 1.5f;

    static const char *const types[] = {"Other", "Call", "Mail", "SNS"};
    auto field = [&](const char *label, const std::string &value, int edit_field) {
        const Rect row = {x, cursor - scroll, w, 72.0f * d};
        draw_card(canvas, row, 12.0f * d);
        canvas.text(label, row.x + 16.0f * d, row.y + 12.0f * d,
                    layout.small_size, palette.text_secondary);
        canvas.text(truncateToWidth(canvas, value, w - 120.0f * d,
                                    layout.text_size),
                    row.x + 16.0f * d, row.y + 38.0f * d, layout.text_size,
                    palette.text_primary);
        if (edit_field >= 0) {
            addHit(row, Action::EditField, {}, edit_field);
        }
        cursor += row.h + gap;
    };
    field("Type", types[notify_type_ & 3], -1);
    field("App", notify_app_, EDIT_NOTIFY_APP);
    field("Title", notify_title_, EDIT_NOTIFY_TITLE);
    field("Text", notify_text_, EDIT_NOTIFY_TEXT);

    button({x, cursor - scroll, half, layout.action_h}, "Change type",
           Action::NotifyType);
    button({x + half + gap, cursor - scroll, half, layout.action_h}, "Send now",
           Action::NotifySend, ButtonVariant::Primary);
    cursor += layout.action_h + gap;
    cursor = drawNote(canvas, layout,
                      "The 0x30 packet has no readback; confirm delivery on the watch. Its notification setting must be enabled. Notification text stays on this device and is never uploaded.",
                      x, cursor - scroll, palette.text_secondary) +
             scroll;
    return cursor - top;
}

} // namespace wristctl::gui
