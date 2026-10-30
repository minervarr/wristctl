#include "ui_layout.hh"

#include <algorithm>

namespace wristctl::gui {

UiMetrics make_ui_metrics(float width, float height, bool touch,
                          float density) noexcept
{
    const float d = density > 0.0f ? density : 1.0f;
    UiMetrics metrics{};
    metrics.density = d;
    metrics.side_navigation = !touch;
    metrics.content_max_width = 1040.0f;
    metrics.margin = touch ? 16.0f * d : 24.0f;
    metrics.spacing = touch ? 12.0f * d : 16.0f;
    metrics.card_radius = touch ? 12.0f * d : 12.0f;
    metrics.title_size = touch ? 26.0f * d : 28.0f;
    metrics.section_size = touch ? 20.0f * d : 20.0f;
    metrics.body_size = touch ? 17.0f * d : 17.0f;
    metrics.secondary_size = touch ? 14.0f * d : 14.0f;
    metrics.hero_size = touch ? 42.0f * d : 56.0f;
    metrics.button_height = touch ? 48.0f * d : 40.0f;
    metrics.row_height = touch ? 56.0f * d : 48.0f;

    if (touch) {
        const float header_height = 56.0f * d;
        const float navigation_height = 72.0f * d;
        metrics.navigation = {0.0f, height - navigation_height, width,
                              navigation_height};
        metrics.header = {0.0f, 0.0f, width, header_height};
        const float content_width = std::max(1.0f, width - 2.0f * metrics.margin);
        metrics.content = {metrics.margin, header_height + metrics.margin,
                           content_width,
                           std::max(1.0f, height - header_height -
                                               navigation_height -
                                               2.0f * metrics.margin)};
    } else {
        constexpr float navigation_width = 200.0f;
        constexpr float header_height = 64.0f;
        metrics.navigation = {0.0f, 0.0f, navigation_width, height};
        metrics.header = {navigation_width, 0.0f,
                          std::max(0.0f, width - navigation_width),
                          header_height};
        const float available = std::max(1.0f, width - navigation_width -
                                                    2.0f * metrics.margin);
        const float content_width = std::min(available, metrics.content_max_width);
        const float content_x = navigation_width +
                                (width - navigation_width - content_width) * 0.5f;
        metrics.content = {content_x, header_height + metrics.margin,
                           content_width,
                           std::max(1.0f, height - header_height -
                                               2.0f * metrics.margin)};
    }
    return metrics;
}

} // namespace wristctl::gui
