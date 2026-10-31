#include "watch_view.hh"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "canvas.hh"
#include "sntp_client.hh"
#include "watch_model.hh"

namespace wristctl::gui {

float WatchView::drawSectionTitle(Canvas &canvas, const Layout &layout,
                                  const std::string &title, float x, float y)
{
    canvas.text(title, x, y, layout.page_title_size, col::text);
    return y + layout.page_title_size * 1.6f;
}

float WatchView::drawKeyValue(Canvas &canvas, const Layout &layout,
                              const std::string &label,
                              const std::string &value, float x, float y,
                              Color color)
{
    canvas.text(label, x, y, layout.small_size, col::dim);
    canvas.text(truncateToWidth(canvas, value,
                                layout.content_w - layout.label_w,
                                layout.text_size),
                x + layout.label_w, y, layout.text_size, color);
    return y + layout.row_line;
}

float WatchView::drawNote(Canvas &canvas, const Layout &layout,
                          const std::string &text, float x, float y,
                          Color color)
{
    return drawWrapped(canvas, text, x, y, layout.content_w, layout.small_size,
                       color, layout.small_size * 1.4f);
}

float WatchView::drawHomePage(Canvas &canvas, const Layout &layout)
{
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float scroll = page_scroll(page_);
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = 8 * d;
    const float half = (layout.content_w - gap) / 2;
    float cursor = drawSectionTitle(canvas, layout, "Home", x, top - scroll) +
                   scroll;

    if (settings_.home_show_connection) {
        const std::string state = snapshot_.connected || snapshot_.demo
                                      ? (snapshot_.name.empty() ? "Connected"
                                                                : snapshot_.name)
                                      : "Not connected";
        cursor = drawKeyValue(canvas, layout, "Watch", state, x,
                              cursor - scroll,
                              snapshot_.connected || snapshot_.demo ? col::green
                                                                    : col::dim) +
                 scroll;
        if (snapshot_.has_condition) {
            char condition[96];
            std::snprintf(condition, sizeof condition,
                          "battery level %d  |  %d C  |  MTU %d",
                          snapshot_.battery, snapshot_.temperature, snapshot_.mtu);
            cursor = drawKeyValue(canvas, layout, "Condition", condition, x,
                                  cursor - scroll) +
                     scroll;
        }
        cursor += gap;
    }

    if (settings_.home_show_clock) {
        auto now = std::chrono::system_clock::now();
        if (settings_.use_ntp && snapshot_.has_time_sample) {
            now = snapshot_.time_corrected_at +
                  std::chrono::duration_cast<std::chrono::system_clock::duration>(
                      std::chrono::steady_clock::now() -
                      snapshot_.time_sampled_at);
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
                      settings_.app_time_24h ? "%H:%M:%S" : "%I:%M:%S %p",
                      &calendar);
        cursor = drawKeyValue(canvas, layout, settings_.use_ntp ? "SNTP UTC"
                                                                : "Local time",
                              clock, x, cursor - scroll) +
                 scroll;
        if (settings_.use_ntp && !snapshot_.time_server.empty()) {
            cursor = drawKeyValue(canvas, layout, "Time server",
                                  snapshot_.time_server, x, cursor - scroll) +
                     scroll;
        }
        cursor += gap;
    }

    if (settings_.home_show_alarms) {
        unsigned enabled = 0;
        if (snapshot_.has_alarms) {
            enabled = snapshot_.alarm_first.enabled ? 1U : 0U;
            for (const auto &alarm : snapshot_.alarm_rest.alarms) {
                enabled += alarm.enabled ? 1U : 0U;
            }
        }
        cursor = drawKeyValue(canvas, layout, "Alarms",
                              snapshot_.has_alarms
                                  ? std::to_string(enabled) + " of 5 enabled"
                                  : "Not read yet",
                              x, cursor - scroll) +
                 scroll;
        addButton(canvas, x, cursor - scroll, half, layout.action_h,
                  "Edit alarms", Action::Page, 3, false,
                  touch_ ? layout.action_h * 0.2f : 7.0f);
        cursor += layout.action_h + gap;
    }

    if (settings_.home_show_timer && snapshot_.has_timer) {
        cursor = drawKeyValue(canvas, layout, "Timer round",
                              duration_text(timer_round_seconds(snapshot_.timer_value)),
                              x, cursor - scroll) +
                 scroll;
        addButton(canvas, x, cursor - scroll, half, layout.action_h,
                  "Edit program timer", Action::Page, 4, false,
                  touch_ ? layout.action_h * 0.2f : 7.0f);
        cursor += layout.action_h + gap;
    }

    cursor = drawSectionTitle(canvas, layout, "Dashboard cards", x,
                              cursor - scroll) +
             scroll;
    auto card_toggle = [&](const char *label, bool on, int card) {
        addButton(canvas, x, cursor - scroll, layout.content_w, layout.action_h,
                  std::string(label) + (on ? ": shown" : ": hidden"),
                  Action::ToggleHomeCard, 0, on,
                  touch_ ? layout.action_h * 0.2f : 7.0f, card);
        cursor += layout.action_h + gap;
    };
    card_toggle("Connection", settings_.home_show_connection, 0);
    card_toggle("Clock", settings_.home_show_clock, 1);
    card_toggle("Alarms", settings_.home_show_alarms, 2);
    card_toggle("Timer", settings_.home_show_timer, 3);

    cursor = drawNote(canvas, layout,
                      "Offline by design. No account, analytics, cloud sync, or "
                      "external fitness service. SNTP is used only when enabled "
                      "for time synchronization.",
                      x, cursor - scroll, col::dim) +
             scroll;
    return cursor - top;
}

float WatchView::drawTrainingPage(Canvas &canvas, const Layout &layout)
{
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float scroll = page_scroll(page_);
    float cursor = drawSectionTitle(canvas, layout, "Training", x, top - scroll) +
                   scroll;
    cursor = drawNote(canvas, layout,
                      "Program timer is available now. It supports six timed "
                      "steps, repeat count, and per-step sound.",
                      x, cursor - scroll, col::text) +
             scroll + layout.row_line;
    addButton(canvas, x, cursor - scroll, layout.content_w, layout.action_h,
              "Open program timer", Action::Page, 4, false,
              touch_ ? layout.action_h * 0.2f : 7.0f);
    cursor += layout.action_h * 1.5f;
    cursor = drawSectionTitle(canvas, layout, "Run measurement", x,
                              cursor - scroll) +
             scroll;
    cursor = drawNote(canvas, layout,
                      "Auto lap, auto pause, training faces, achievement alerts, "
                      "and lap faces are not enabled yet: their wire formats "
                      "still require evidence-backed codecs. No placeholder "
                      "writes are sent to the watch.",
                      x, cursor - scroll, col::dim) +
             scroll;
    return cursor - top;
}

float WatchView::drawActivityPage(Canvas &canvas, const Layout &layout)
{
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float scroll = page_scroll(page_);
    float cursor = drawSectionTitle(canvas, layout, "Activity", x, top - scroll) +
                   scroll;
    cursor = drawNote(canvas, layout,
                      "No locally stored activity sessions.",
                      x, cursor - scroll, col::text) +
             scroll + layout.row_line;
    cursor = drawNote(canvas, layout,
                      "History, analysis, records, goals, and achievements will "
                      "use watch data stored on this device only. Activity "
                      "download is not available until the convoy transfer "
                      "protocol is decoded; this page never contacts an online "
                      "coaching or fitness service.",
                      x, cursor - scroll, col::dim) +
             scroll;
    return cursor - top;
}

} // namespace wristctl::gui
