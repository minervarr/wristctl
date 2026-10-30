#include "watch_view.hh"

#include <chrono>
#include <cstdio>
#include <ctime>

#include "sntp_client.hh"
#include "ui_components.hh"
#include "ui_theme.hh"
#include "watch_model.hh"

namespace wristctl::gui {

float WatchView::drawOverviewPage(Canvas &canvas, const Layout &layout)
{
    const auto &palette = dark_palette();
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float scroll = page_scroll(route_index(route_));
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = touch_ ? 12.0f * d : 16.0f;
    float cursor = top;

    canvas.text("Your watch, at a glance", x, cursor - scroll,
                layout.page_title_size, palette.text_primary);
    cursor += layout.page_title_size + 8.0f * d;
    canvas.text("The essentials, with focused tools one tap away.", x,
                cursor - scroll, layout.small_size, palette.text_secondary);
    cursor += layout.small_size + 20.0f * d;

    if (snapshot_.demo) {
        const std::string message = demo_scenario_ == DemoScenario::Recorded
            ? "Recorded demo. Explore freely; no watch is required."
            : "Scenario preview: " + std::string(demo_scenario_name(demo_scenario_)) +
                  ". Watch actions are paused.";
        draw_banner(canvas, {x, cursor - scroll, layout.content_w, 42.0f * d},
                    truncateToWidth(canvas, message, layout.content_w - 28.0f * d,
                                    layout.small_size),
                    layout.small_size, palette.accent);
        cursor += 54.0f * d;
    }
    if (!snapshot_.error.empty()) {
        draw_banner(canvas, {x, cursor - scroll, layout.content_w, 42.0f * d},
                    truncateToWidth(canvas, snapshot_.error,
                                    layout.content_w - 28.0f * d,
                                    layout.small_size),
                    layout.small_size, palette.error);
        cursor += 54.0f * d;
    } else if (demo_scenario_ == DemoScenario::Success) {
        draw_banner(canvas, {x, cursor - scroll, layout.content_w, 42.0f * d},
                    snapshot_.status, layout.small_size, palette.success);
        cursor += 54.0f * d;
    }

    struct Summary {
        bool visible;
        Route route;
        std::string title;
        std::string value;
        std::string detail;
        Color color;
    };
    Summary summaries[4];
    int summary_count = 0;

    if (settings_.home_show_connection) {
        const std::string name = snapshot_.name.empty() ? "Your watch" : snapshot_.name;
        std::string detail = snapshot_.connected
            ? (snapshot_.model.empty() ? "Ready when you are" : snapshot_.model)
            : "Open Watch to connect or pair";
        if (snapshot_.has_condition) {
            detail += " · " + std::to_string(snapshot_.battery) + "% battery";
        }
        summaries[summary_count++] = {
            true, Route::Watch, name,
            snapshot_.connected ? "Connected" : "Not connected", detail,
            snapshot_.connected ? palette.success : palette.text_secondary};
    }

    if (settings_.home_show_clock) {
        auto now = std::chrono::system_clock::now();
        if (settings_.use_ntp && snapshot_.has_time_sample) {
            now = snapshot_.time_corrected_at +
                  std::chrono::duration_cast<std::chrono::system_clock::duration>(
                      std::chrono::steady_clock::now() - snapshot_.time_sampled_at);
        }
        std::tm calendar{};
        if (settings_.use_ntp) {
            calendar = utc_tm(now);
        } else {
            const std::time_t time = std::chrono::system_clock::to_time_t(now);
            localtime_r(&time, &calendar);
        }
        char clock[32];
        std::strftime(clock, sizeof clock,
                      settings_.app_time_24h ? "%H:%M" : "%I:%M %p", &calendar);
        std::string source = settings_.use_ntp ? "SNTP UTC" : "Device local time";
        source += settings_.sync_on_connect ? " · sync on connect" : " · manual sync";
        summaries[summary_count++] = {true, Route::Time, "Time", clock, source,
                                      palette.text_primary};
    }

    if (settings_.home_show_alarms) {
        unsigned enabled = 0;
        if (snapshot_.has_alarms) {
            enabled = snapshot_.alarm_first.enabled ? 1U : 0U;
            for (const auto &alarm : snapshot_.alarm_rest.alarms) {
                enabled += alarm.enabled ? 1U : 0U;
            }
        }
        summaries[summary_count++] = {
            true, Route::Alarms, "Alarms",
            snapshot_.has_alarms ? std::to_string(enabled) + " of 5 on" : "No data yet",
            snapshot_.has_alarms ? "Manage times, repeat, and chime"
                                 : "Connect to read your alarms",
            enabled != 0 ? palette.success : palette.text_secondary};
    }

    if (settings_.home_show_timer) {
        summaries[summary_count++] = {
            true, Route::Timer, "Program timer",
            snapshot_.has_timer
                ? duration_text(timer_round_seconds(snapshot_.timer_value))
                : "No data yet",
            snapshot_.has_timer ? "One round · six customizable steps"
                                : "Connect to read your timer",
            palette.text_primary};
    }

    if (summary_count == 0) {
        const Rect empty = {x, cursor - scroll, layout.content_w, 180.0f * d};
        draw_card(canvas, empty);
        draw_empty_state(canvas, empty, "A quieter Overview",
                         "Choose visible cards in Settings.",
                         layout.date_size, layout.small_size);
        cursor += 180.0f * d + gap;
    } else {
        const bool two_columns = layout.content_w >= 520.0f * d;
        const int columns = two_columns ? 2 : 1;
        const float card_w = (layout.content_w - gap * (columns - 1)) / columns;
        const float card_h = touch_ ? 150.0f * d : 156.0f;
        for (int index = 0; index < summary_count; ++index) {
            const Summary &summary = summaries[index];
            const int column = index % columns;
            const int row = index / columns;
            const Rect card = {x + column * (card_w + gap),
                               cursor + row * (card_h + gap) - scroll, card_w,
                               card_h};
            draw_card(canvas, card, touch_ ? 12.0f * d : 12.0f);
            const float pad = touch_ ? 16.0f * d : 18.0f;
            canvas.text(truncateToWidth(canvas, summary.title,
                                        card.w - 2.0f * pad, layout.small_size),
                        card.x + pad, card.y + pad, layout.small_size,
                        palette.text_secondary);
            const float value_size = touch_ ? 24.0f * d : 26.0f;
            canvas.text(truncateToWidth(canvas, summary.value,
                                        card.w - 2.0f * pad, value_size),
                        card.x + pad, card.y + pad + 28.0f * d, value_size,
                        summary.color);
            drawWrapped(canvas, summary.detail, card.x + pad,
                        card.y + pad + 68.0f * d, card.w - 2.0f * pad,
                        layout.small_size, palette.text_secondary,
                        layout.small_size * 1.4f);
            canvas.text("Open", card.x + pad, card.y + card.h - pad - layout.small_size,
                        layout.small_size, palette.accent);
            addHit(card, Action::Page, {}, route_index(summary.route));
        }
        const int rows = (summary_count + columns - 1) / columns;
        cursor += rows * card_h + (rows - 1) * gap + gap;
    }

    cursor = drawNote(canvas, layout,
                      "Private by default: no account, analytics, or cloud sync.",
                      x, cursor - scroll, palette.text_secondary) +
             scroll;
    return cursor - top;
}

} // namespace wristctl::gui
