#include "watch_view.hh"

#include <cstdio>

#include "ui_components.hh"
#include "ui_theme.hh"

namespace wristctl::gui {
namespace {

std::string frame_text(const wc_frame &frame)
{
    if (frame.len == 0) {
        return "not read";
    }
    char bytes[WC_FRAME_MAX * 3 + 1]{};
    size_t used = 0;
    for (uint8_t index = 0; index < frame.len && index < WC_FRAME_MAX; ++index) {
        used += static_cast<size_t>(std::snprintf(
            bytes + used, sizeof bytes - used, "%s%02x", index == 0 ? "" : " ",
            frame.data[index]));
    }
    return bytes;
}

} // namespace

float WatchView::drawDiagnosticsPage(Canvas &canvas, const Layout &layout)
{
    const auto &palette = dark_palette();
    const float scroll = page_scroll(route_index(route_));
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = touch_ ? 10.0f * d : 12.0f;
    float cursor = top;

    canvas.text("Diagnostics", x, cursor - scroll, layout.page_title_size,
                palette.text_primary);
    cursor += layout.page_title_size + 8.0f * d;
    canvas.text("Read-only technical details and reproducible demo states.", x,
                cursor - scroll, layout.small_size, palette.text_secondary);
    cursor += layout.small_size + 18.0f * d;

    if (snapshot_.demo) {
        draw_section_title(canvas, "Demo scenarios", x, cursor - scroll,
                           layout.date_size);
        cursor += layout.date_size + 12.0f * d;
        if (demo_scenario_ != DemoScenario::Recorded) {
            draw_banner(canvas, {x, cursor - scroll, layout.content_w, 42.0f * d},
                        "Preview only—no watch actions.", layout.small_size,
                        palette.warning);
            cursor += 52.0f * d;
        }
        constexpr DemoScenario scenarios[] = {
            DemoScenario::Recorded, DemoScenario::Disconnected,
            DemoScenario::Scanning, DemoScenario::Permission, DemoScenario::Empty,
            DemoScenario::Dirty, DemoScenario::Busy, DemoScenario::Confirmation,
            DemoScenario::Success, DemoScenario::Error};
        const int columns = layout.content_w >= 600.0f * d ? 5 : 2;
        const float button_w =
            (layout.content_w - gap * static_cast<float>(columns - 1)) / columns;
        for (int index = 0; index < 10; ++index) {
            const int column = index % columns;
            const int row = index / columns;
            const Rect rect = {x + column * (button_w + gap),
                               cursor + row * (layout.action_h + gap) - scroll,
                               button_w, layout.action_h};
            draw_button(canvas, rect, demo_scenario_name(scenarios[index]),
                        demo_scenario_ == scenarios[index]
                            ? ButtonVariant::Primary
                            : ButtonVariant::Secondary,
                        layout.small_size);
            hits_.push_back({rect, Action::DemoScenario, 0, {}, index, 0});
        }
        const int rows = (10 + columns - 1) / columns;
        cursor += rows * layout.action_h + (rows - 1) * gap + 20.0f * d;
    }

    draw_section_title(canvas, "Watch data", x, cursor - scroll, layout.date_size);
    cursor += layout.date_size + 12.0f * d;
    cursor = drawKeyValue(canvas, layout, "Module frame",
                          frame_text(snapshot_.module_frame), x, cursor - scroll) +
             scroll;
    cursor = drawKeyValue(canvas, layout, "Version frame",
                          frame_text(snapshot_.version_frame), x, cursor - scroll) +
             scroll;
    char flags[32];
    std::snprintf(flags, sizeof flags, "0x%02x", snapshot_.settings_flags);
    cursor = drawKeyValue(canvas, layout, "Settings flags",
                          snapshot_.has_settings ? flags : "not read", x,
                          cursor - scroll) +
             scroll;
    cursor = drawKeyValue(canvas, layout, "Pairing",
                          snapshot_.pairing.empty() ? "not read" : snapshot_.pairing,
                          x, cursor - scroll) +
             scroll;
    cursor = drawKeyValue(canvas, layout, "MTU",
                          snapshot_.has_condition
                              ? std::to_string(snapshot_.mtu)
                              : "not read",
                          x, cursor - scroll) +
             scroll + gap;

    draw_section_title(canvas, "Time synchronization", x, cursor - scroll,
                       layout.date_size);
    cursor += layout.date_size + 12.0f * d;
    cursor = drawKeyValue(canvas, layout, "SNTP server",
                          snapshot_.time_server.empty() ? "not sampled"
                                                        : snapshot_.time_server,
                          x, cursor - scroll) +
             scroll;
    if (snapshot_.has_time_sample) {
        char sample[128];
        std::snprintf(sample, sizeof sample, "offset %.3f ms · RTT %.3f ms",
                      snapshot_.time_offset_ms, snapshot_.time_round_trip_ms);
        cursor = drawKeyValue(canvas, layout, "Last sample", sample, x,
                              cursor - scroll) +
                 scroll;
    }
    if (!snapshot_.time_error.empty()) {
        cursor = drawNote(canvas, layout, snapshot_.time_error, x, cursor - scroll,
                          palette.error) +
                 scroll;
    }
    cursor += gap;

    draw_section_title(canvas, "Activity log", x, cursor - scroll, layout.date_size);
    cursor += layout.date_size + 12.0f * d;
    const auto &log = snapshot_.log;
    if (log.empty()) {
        cursor = drawNote(canvas, layout, "No diagnostic messages yet.", x,
                          cursor - scroll, palette.text_secondary) +
                 scroll;
    } else {
        const size_t limit = touch_ ? 40 : 24;
        const size_t start = log.size() > limit ? log.size() - limit : 0;
        const float line = layout.small_size * 1.45f;
        for (size_t index = start; index < log.size(); ++index) {
            const std::string shown = truncateToWidth(
                canvas, log[index], layout.content_w, layout.small_size);
            canvas.text(shown, x, cursor - scroll, layout.small_size,
                        palette.text_secondary);
            cursor += line;
        }
    }
    return cursor - top;
}

} // namespace wristctl::gui
