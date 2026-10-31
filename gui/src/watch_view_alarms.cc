#include "watch_view.hh"

#include <algorithm>
#include <cstdio>

#include "ui_components.hh"
#include "ui_theme.hh"
#include "watch_model.hh"

namespace wristctl::gui {
namespace {

std::string alarm_time(uint8_t hour, uint8_t minute)
{
    char text[16];
    std::snprintf(text, sizeof text, "%02u:%02u", hour, minute);
    return text;
}

} // namespace

float WatchView::drawAlarmsPage(Canvas &canvas, const Layout &layout)
{
    const auto &palette = dark_palette();
    const float scroll = page_scroll(route_index(route_));
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float w = layout.content_w;
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = touch_ ? 10.0f * d : 12.0f;
    const float pad = 18.0f * d;
    float cursor = top;

    canvas.text("Alarms", x, cursor - scroll, layout.page_title_size,
                palette.text_primary);
    cursor += layout.page_title_size + 8.0f * d;
    char summary[64];
    std::snprintf(summary, sizeof summary, "%u of 5 enabled",
                  alarm_enabled_count(edit_first_, edit_rest_));
    canvas.text(summary, x, cursor - scroll, layout.small_size,
                palette.text_secondary);
    cursor += layout.small_size + 18.0f * d;

    if (!snapshot_.has_alarms) {
        const Rect empty = {x, cursor - scroll, w, 144.0f * d};
        draw_card(canvas, empty, 12.0f * d);
        draw_empty_state(canvas, empty, "Alarms not read yet",
                         "Connect and refresh before editing.",
                         layout.date_size, layout.small_size);
        return empty.h + gap;
    }

    auto button = [&](const Rect &rect, const std::string &label, Action action,
                      ButtonVariant variant = ButtonVariant::Secondary,
                      int arg = 0, int arg2 = 0, bool enabled = true) {
        draw_button(canvas, rect, label, variant, layout.small_size, enabled);
        if (enabled) {
            hits_.push_back({rect, action, 0, {}, arg, arg2});
        }
    };

    for (int index = 0; index < 5; ++index) {
        const bool first = index == 0;
        const auto &alarm = edit_rest_.alarms[first ? 0 : index - 1];
        const uint8_t enabled = first ? edit_first_.enabled : alarm.enabled;
        const uint8_t one_time = first ? edit_first_.one_time : alarm.one_time;
        const uint8_t hour = first ? edit_first_.hour : alarm.hour;
        const uint8_t minute = first ? edit_first_.minute : alarm.minute;

        const Rect card = {x, cursor - scroll, w, 112.0f * d};
        draw_card(canvas, card, 12.0f * d);
        char label[32];
        std::snprintf(label, sizeof label, "Alarm %d", index + 1);
        canvas.text(label, card.x + pad, card.y + 14.0f * d,
                    layout.small_size, palette.text_secondary);
        canvas.text(alarm_time(hour, minute), card.x + pad,
                    card.y + 42.0f * d, layout.date_size,
                    palette.text_primary);
        std::string state = enabled ? "Enabled" : "Disabled";
        state += one_time ? " · once" : " · daily";
        if (first) {
            state += edit_first_.hourly_chime ? " · hourly chime"
                                              : " · chime off";
        }
        canvas.text(truncateToWidth(canvas, state, w - 150.0f * d,
                                    layout.small_size),
                    card.x + pad, card.y + 78.0f * d, layout.small_size,
                    palette.text_secondary);
        const Rect open = {card.x, card.y, card.w - 116.0f * d, card.h};
        addHit(open, Action::AlarmEdit, {}, index);
        button({card.x + card.w - 104.0f * d, card.y + 16.0f * d,
                88.0f * d, layout.action_h},
               enabled ? "On" : "Off", Action::AlarmToggle,
               enabled ? ButtonVariant::Primary : ButtonVariant::Secondary,
               index);
        cursor += card.h + gap;
    }

    const float half = (w - gap) * 0.5f;
    button({x, cursor - scroll, half, layout.action_h},
           alarms_dirty_
               ? (alarms_confirm_ ? "Confirm two-frame write" : "Apply all alarms")
               : "No alarm changes",
           Action::AlarmApply, ButtonVariant::Primary, 0, 0, alarms_dirty_);
    button({x + half + gap, cursor - scroll, half, layout.action_h},
           alarms_dirty_ ? "Revert draft" : "Draft clean",
           Action::AlarmRevert, ButtonVariant::Secondary, 0, 0, alarms_dirty_);
    cursor += layout.action_h + gap;
    cursor = drawNote(canvas, layout,
                      "Open a card to edit time and mode. Apply writes both alarm frames after confirmation.",
                      x, cursor - scroll, palette.text_secondary) +
             scroll;
    return cursor - top;
}

float WatchView::drawAlarmEditor(Canvas &canvas, const Layout &layout)
{
    const auto &palette = dark_palette();
    const float scroll = page_scroll(route_index(route_));
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float w = layout.content_w;
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = touch_ ? 10.0f * d : 12.0f;
    const float half = (w - gap) * 0.5f;
    const float pad = 18.0f * d;
    const int index = detail_index_;
    const bool first = index == 0;
    const auto &alarm = edit_rest_.alarms[first ? 0 : index - 1];
    const uint8_t enabled = first ? edit_first_.enabled : alarm.enabled;
    const uint8_t one_time = first ? edit_first_.one_time : alarm.one_time;
    const uint8_t hour = first ? edit_first_.hour : alarm.hour;
    const uint8_t minute = first ? edit_first_.minute : alarm.minute;
    float cursor = top;

    auto button = [&](const Rect &rect, const std::string &label, Action action,
                      ButtonVariant variant = ButtonVariant::Secondary,
                      int arg = 0, int arg2 = 0, bool active = true) {
        draw_button(canvas, rect, label, variant, layout.small_size, active);
        if (active) {
            hits_.push_back({rect, action, 0, {}, arg, arg2});
        }
    };

    button({x, cursor - scroll, 96.0f * d, layout.action_h}, "Back",
           Action::CloseDetail, ButtonVariant::Quiet);
    char title[32];
    std::snprintf(title, sizeof title, "Edit alarm %d", index + 1);
    canvas.text(title, x + 112.0f * d,
                cursor - scroll +
                    (layout.action_h - layout.page_title_size) * 0.5f,
                layout.page_title_size, palette.text_primary);
    cursor += layout.action_h + gap * 1.5f;

    const Rect hero = {x, cursor - scroll, w, layout.clock_size + 88.0f * d};
    draw_card(canvas, hero, 14.0f * d);
    canvas.text(enabled ? "Enabled" : "Disabled", hero.x + pad,
                hero.y + pad, layout.small_size, palette.text_secondary);
    canvas.text(alarm_time(hour, minute), hero.x + pad,
                hero.y + pad + 26.0f * d, layout.clock_size,
                palette.text_primary);
    canvas.text(one_time ? "Once" : "Daily", hero.x + pad,
                hero.y + pad + 32.0f * d + layout.clock_size,
                layout.date_size, palette.text_secondary);
    cursor += hero.h + gap;

    button({x, cursor - scroll, w, layout.action_h},
           enabled ? "Disable alarm" : "Enable alarm", Action::AlarmToggle,
           ButtonVariant::Secondary, index);
    cursor += layout.action_h + gap;

    canvas.text("Hour", x, cursor - scroll, layout.small_size,
                palette.text_secondary);
    cursor += layout.small_size + 8.0f * d;
    button({x, cursor - scroll, half, layout.action_h}, "−1 hour",
           Action::AlarmHour, ButtonVariant::Secondary, index, -1);
    button({x + half + gap, cursor - scroll, half, layout.action_h},
           "+1 hour", Action::AlarmHour, ButtonVariant::Secondary, index, 1);
    cursor += layout.action_h + gap;

    canvas.text("Minute", x, cursor - scroll, layout.small_size,
                palette.text_secondary);
    cursor += layout.small_size + 8.0f * d;
    button({x, cursor - scroll, half, layout.action_h}, "−5 minutes",
           Action::AlarmMinute, ButtonVariant::Secondary, index, -5);
    button({x + half + gap, cursor - scroll, half, layout.action_h},
           "+5 minutes", Action::AlarmMinute, ButtonVariant::Secondary,
           index, 5);
    cursor += layout.action_h + gap;

    button({x, cursor - scroll, w, layout.action_h},
           one_time ? "Repeats: once" : "Repeats: daily",
           Action::AlarmMode, ButtonVariant::Secondary, index);
    cursor += layout.action_h + gap;
    if (first) {
        button({x, cursor - scroll, w, layout.action_h},
               edit_first_.hourly_chime ? "Hourly chime: on"
                                        : "Hourly chime: off",
               Action::AlarmChime);
        cursor += layout.action_h + gap;
    }

    button({x, cursor - scroll, half, layout.action_h},
           alarms_dirty_
               ? (alarms_confirm_ ? "Confirm alarm write" : "Apply alarms")
               : "No alarm changes",
           Action::AlarmApply, ButtonVariant::Primary, 0, 0, alarms_dirty_);
    button({x + half + gap, cursor - scroll, half, layout.action_h},
           alarms_dirty_ ? "Revert draft" : "Draft clean",
           Action::AlarmRevert, ButtonVariant::Secondary, 0, 0, alarms_dirty_);
    cursor += layout.action_h + gap;
    cursor = drawNote(canvas, layout,
                      "Back keeps this draft and confirmation. Apply writes both frames and reads the alarms back.",
                      x, cursor - scroll, palette.text_secondary) +
             scroll;
    return cursor - top;
}

} // namespace wristctl::gui
