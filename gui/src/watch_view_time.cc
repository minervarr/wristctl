#include "watch_view.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "sntp_client.hh"
#include "ui_components.hh"
#include "ui_theme.hh"
#include "watch_model.hh"

namespace wristctl::gui {

float WatchView::drawTimePage(Canvas &canvas, const Layout &layout)
{
    auto display_now = std::chrono::system_clock::now();
    const bool corrected = settings_.use_ntp && snapshot_.has_time_sample;
    if (corrected) {
        display_now = snapshot_.time_corrected_at +
                      std::chrono::duration_cast<
                          std::chrono::system_clock::duration>(
                          std::chrono::steady_clock::now() -
                          snapshot_.time_sampled_at);
    }
    std::tm calendar{};
    if (settings_.use_ntp) {
        calendar = utc_tm(display_now);
    } else {
        const std::time_t seconds =
            std::chrono::system_clock::to_time_t(display_now);
        localtime_r(&seconds, &calendar);
    }

    char clock[32];
    char date[64];
    std::strftime(clock, sizeof clock,
                  settings_.app_time_24h ? "%H:%M:%S" : "%I:%M:%S %p",
                  &calendar);
    std::strftime(date, sizeof date, "%A %d %B %Y", &calendar);

    std::string zone_label;
    if (corrected) {
        zone_label = "SNTP UTC · " + snapshot_.time_server;
    } else if (settings_.use_ntp) {
        zone_label = "UTC preview · device clock until SNTP succeeds";
    } else {
        const char *zone = std::getenv("TZ");
        zone_label = zone != nullptr && *zone != '\0'
            ? std::string("Device local · ") + zone
            : std::string(touch_ ? "Device local time" : "Host local time");
    }

    const auto &palette = dark_palette();
    const float scroll = page_scroll(route_index(route_));
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float w = layout.content_w;
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = touch_ ? 10.0f * d : 12.0f;
    const float pad = 18.0f * d;
    float cursor = top;

    auto button = [&](const Rect &rect, const std::string &label, Action action,
                      ButtonVariant variant = ButtonVariant::Secondary,
                      int arg = 0, bool enabled = true) {
        draw_button(canvas, rect, label, variant, layout.small_size, enabled);
        if (enabled) {
            hits_.push_back({rect, action, 0, {}, arg, 0});
        }
    };

    const Rect hero = {x, cursor - scroll, w, layout.clock_size + 92.0f * d};
    draw_card(canvas, hero, 14.0f * d);
    canvas.text("Time", hero.x + pad, hero.y + pad, layout.small_size,
                palette.text_secondary);
    canvas.text(clock, hero.x + pad, hero.y + pad + 26.0f * d,
                layout.clock_size, palette.text_primary);
    canvas.text(date, hero.x + pad, hero.y + pad + 32.0f * d + layout.clock_size,
                layout.date_size, palette.text_secondary);
    cursor += hero.h + gap;
    cursor = drawWrapped(canvas, zone_label, x, cursor - scroll, w,
                         layout.small_size, palette.text_secondary,
                         layout.small_size * 1.4f) +
             scroll + gap;

    button({x, cursor - scroll, w, layout.action_h}, "Sync now",
           Action::SyncTime, ButtonVariant::Primary);
    cursor += layout.action_h + gap;
    const float half = (w - gap) * 0.5f;
    button({x, cursor - scroll, half, layout.action_h},
           settings_.use_ntp ? "Source: SNTP UTC" : "Source: device local",
           Action::ToggleTimeSource);
    button({x + half + gap, cursor - scroll, half, layout.action_h},
           settings_.sync_on_connect ? "Sync on connect: on"
                                     : "Sync on connect: off",
           Action::ToggleSyncOnConnect);
    cursor += layout.action_h + gap * 1.5f;

    draw_section_title(canvas, "Display format", x, cursor - scroll,
                       layout.date_size);
    cursor += layout.date_size + 12.0f * d;
    if (snapshot_.has_settings) {
        const bool show_24h = edit_settings_.time_format_24h != 0;
        button({x, cursor - scroll, half, layout.action_h},
               show_24h ? "Watch: 24-hour" : "Watch: 12-hour",
               Action::SettingToggle);
        button({x + half + gap, cursor - scroll, half, layout.action_h},
               settings_dirty_ ? "Apply watch format" : "Format unchanged",
               Action::SettingsApply, ButtonVariant::Secondary, 0,
               settings_dirty_);
        cursor += layout.action_h + gap;
    }
    button({x, cursor - scroll, w, layout.action_h},
           settings_.app_time_24h ? "App: 24-hour" : "App: 12-hour",
           Action::ToggleAppTimeFormat);
    cursor += layout.action_h + gap * 1.5f;

    draw_section_title(canvas, "Cities & daylight saving", x, cursor - scroll,
                       layout.date_size);
    cursor += layout.date_size + 12.0f * d;
    if (!snapshot_.has_clock) {
        const Rect empty = {x, cursor - scroll, w, 120.0f * d};
        draw_card(canvas, empty, 12.0f * d);
        draw_empty_state(canvas, empty, "Cities not read yet",
                         "Connect and refresh to edit city time.",
                         layout.date_size, layout.small_size);
        cursor += empty.h + gap;
    } else {
        static const char *const units[] = {"Home", "World", "City 3",
                                            "City 4", "City 5"};
        for (int unit = 0; unit < 5; ++unit) {
            const Rect row = {x, cursor - scroll, w, 76.0f * d};
            draw_card(canvas, row, 12.0f * d);
            canvas.text(units[unit], row.x + pad, row.y + 12.0f * d,
                        layout.small_size, palette.text_secondary);
            const std::string name = unit == clock_unit_ && clock_dirty_
                ? edit_city_name_
                : std::string(edit_city_names_[unit].name);
            canvas.text(truncateToWidth(canvas, name.empty() ? "Unnamed city" : name,
                                        w * 0.58f, layout.text_size),
                        row.x + pad, row.y + 36.0f * d, layout.text_size,
                        palette.text_primary);
            const std::string offset = offset_text(
                edit_dst_settings_[unit].timezone, true);
            canvas.text(offset, row.x + row.w - pad -
                                    canvas.textWidth(offset, layout.text_size),
                        row.y + 36.0f * d, layout.text_size,
                        palette.text_primary);
            if (unit == clock_unit_ && clock_dirty_) {
                draw_badge(canvas, "Edited", row.x + row.w - 82.0f * d,
                           row.y + 10.0f * d, layout.small_size, palette.accent);
            }
            addHit(row, Action::ClockUnit, {}, unit);
            cursor += row.h + gap;
        }
        if (clock_dirty_) {
            cursor = drawNote(canvas, layout,
                              "Apply or revert the active city before editing another.",
                              x, cursor - scroll, palette.text_secondary) +
                     scroll + gap;
        }
    }

    draw_section_title(canvas, "Advanced time", x, cursor - scroll,
                       layout.date_size);
    cursor += layout.date_size + 12.0f * d;
    button({x, cursor - scroll, w, layout.action_h}, "Edit SNTP servers",
           Action::EditField, ButtonVariant::Secondary, EDIT_NTP_SERVERS);
    cursor += layout.action_h + gap;
    if (snapshot_.has_time_sample) {
        char sample[128];
        std::snprintf(sample, sizeof sample,
                      "Last offset %.3f ms · round trip %.3f ms",
                      snapshot_.time_offset_ms, snapshot_.time_round_trip_ms);
        cursor = drawNote(canvas, layout, sample, x, cursor - scroll,
                          palette.text_secondary) +
                 scroll;
    }
    if (settings_.use_ntp && !snapshot_.time_error.empty()) {
        cursor = drawNote(canvas, layout, snapshot_.time_error, x,
                          cursor - scroll, palette.error) +
                 scroll;
    }
    return cursor - top;
}

float WatchView::drawCityEditor(Canvas &canvas, const Layout &layout)
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
                      int arg = 0, bool enabled = true) {
        draw_button(canvas, rect, label, variant, layout.small_size, enabled);
        if (enabled) {
            hits_.push_back({rect, action, 0, {}, arg, 0});
        }
    };

    static const char *const units[] = {"Home", "World", "City 3",
                                        "City 4", "City 5"};
    button({x, cursor - scroll, 96.0f * d, layout.action_h}, "Back",
           Action::CloseDetail, ButtonVariant::Quiet);
    canvas.text(std::string("Edit ") + units[clock_unit_],
                x + 112.0f * d, cursor - scroll +
                    (layout.action_h - layout.page_title_size) * 0.5f,
                layout.page_title_size, palette.text_primary);
    cursor += layout.action_h + gap * 1.5f;

    const Rect identity = {x, cursor - scroll, w, 116.0f * d};
    draw_card(canvas, identity, 12.0f * d);
    canvas.text("City label", identity.x + 18.0f * d, identity.y + 16.0f * d,
                layout.small_size, palette.text_secondary);
    canvas.text(truncateToWidth(canvas,
                                edit_city_name_.empty() ? "Unnamed city"
                                                        : edit_city_name_,
                                w - 130.0f * d, layout.date_size),
                identity.x + 18.0f * d, identity.y + 46.0f * d,
                layout.date_size, palette.text_primary);
    button({identity.x + identity.w - 98.0f * d,
            identity.y + 14.0f * d, 80.0f * d, layout.action_h},
           "Edit", Action::EditField, ButtonVariant::Secondary, EDIT_CITY_NAME);
    char city_id[40];
    std::snprintf(city_id, sizeof city_id, "ID 0x%04x · preserved",
                  edit_dst_settings_[clock_unit_].city);
    canvas.text(city_id, identity.x + 18.0f * d,
                identity.y + 82.0f * d, layout.small_size,
                palette.text_secondary);
    cursor += identity.h + gap;

    const wc_dst_setting &city = edit_dst_settings_[clock_unit_];
    cursor = drawKeyValue(canvas, layout, "UTC offset",
                          offset_text(city.timezone, true), x,
                          cursor - scroll) +
             scroll;
    button({x, cursor - scroll, half, layout.action_h}, "−15 minutes",
           Action::CityTimezone, ButtonVariant::Secondary, -1);
    button({x + half + gap, cursor - scroll, half, layout.action_h},
           "+15 minutes", Action::CityTimezone, ButtonVariant::Secondary, 1);
    cursor += layout.action_h + gap;

    cursor = drawKeyValue(canvas, layout, "DST difference",
                          offset_text(city.dst_diff, false), x,
                          cursor - scroll) +
             scroll;
    button({x, cursor - scroll, half, layout.action_h}, "−15 minutes",
           Action::CityDstDiff, ButtonVariant::Secondary, -1);
    button({x + half + gap, cursor - scroll, half, layout.action_h},
           "+15 minutes", Action::CityDstDiff, ButtonVariant::Secondary, 1);
    cursor += layout.action_h + gap;

    if (clock_unit_ < 2) {
        const bool dst_on = clock_unit_ == 0
            ? edit_dst_state_.dst_on_home != 0
            : edit_dst_state_.dst_on_world != 0;
        const bool dst_auto = clock_unit_ == 0
            ? edit_dst_state_.dst_auto_home != 0
            : edit_dst_state_.dst_auto_world != 0;
        button({x, cursor - scroll, half, layout.action_h},
               dst_on ? "DST: on" : "DST: off", Action::CityDstToggle,
               ButtonVariant::Secondary, 0);
        button({x + half + gap, cursor - scroll, half, layout.action_h},
               dst_auto ? "Auto DST: on" : "Auto DST: off",
               Action::CityDstToggle, ButtonVariant::Secondary, 1);
        cursor += layout.action_h + gap;
    }

    button({x, cursor - scroll, half, layout.action_h},
           clock_confirm_ ? "Confirm city write" : "Apply city",
           Action::CityApply, ButtonVariant::Primary, 0, clock_dirty_);
    button({x + half + gap, cursor - scroll, half, layout.action_h},
           "Revert city", Action::CityRevert);
    cursor += layout.action_h + gap;
    cursor = drawWrapped(
                 canvas,
                 "Back keeps this draft. Apply requires a second tap, preserves unread fields, and reads all five city slots back.",
                 x, cursor - scroll, w, layout.small_size,
                 palette.text_secondary, layout.small_size * 1.4f) +
             scroll;
    return cursor - top;
}

} // namespace wristctl::gui
