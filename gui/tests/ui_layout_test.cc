#undef NDEBUG

#include <assert.h>
#include <math.h>

#include "ui_layout.hh"

namespace {

void assert_valid(const wristctl::gui::UiMetrics &metrics)
{
    assert(metrics.content.w > 0.0f);
    assert(metrics.content.h > 0.0f);
    assert(metrics.content.w <= 1040.0f);
    assert(metrics.content.x >= 0.0f);
    assert(metrics.content.y >= metrics.header.y + metrics.header.h);
    assert(metrics.content.y + metrics.content.h <=
           metrics.navigation.y + metrics.navigation.h + 0.001f ||
           metrics.side_navigation);
    assert(metrics.button_height > 0.0f);
    assert(metrics.row_height >= metrics.button_height);
    assert(metrics.margin > 0.0f);
    assert(metrics.spacing > 0.0f);
    assert(metrics.card_radius > 0.0f);
    assert(metrics.title_size > metrics.body_size);
    assert(metrics.body_size > metrics.secondary_size);

    const float values[] = {
        metrics.navigation.x, metrics.navigation.y,
        metrics.navigation.w, metrics.navigation.h,
        metrics.header.x, metrics.header.y, metrics.header.w, metrics.header.h,
        metrics.content.x, metrics.content.y, metrics.content.w, metrics.content.h,
        metrics.content_max_width, metrics.margin, metrics.spacing,
        metrics.card_radius, metrics.title_size, metrics.body_size,
        metrics.secondary_size, metrics.hero_size, metrics.row_height,
        metrics.button_height,
    };
    for (const float value : values) {
        assert(isfinite(value));
    }
}

} // namespace

int main()
{
    using wristctl::gui::make_ui_metrics;

    const auto compact_desktop = make_ui_metrics(960.0f, 720.0f, false, 1.0f);
    assert(compact_desktop.side_navigation);
    assert(compact_desktop.navigation.w == 200.0f);
    assert(compact_desktop.header.h == 64.0f);
    assert(compact_desktop.button_height == 40.0f);
    assert_valid(compact_desktop);

    const auto wide_desktop = make_ui_metrics(1440.0f, 900.0f, false, 1.0f);
    assert(wide_desktop.side_navigation);
    assert(wide_desktop.content.w == 1040.0f);
    assert(wide_desktop.content.x > compact_desktop.content.x);
    assert_valid(wide_desktop);

    const auto touch = make_ui_metrics(320.0f, 640.0f, true, 1.0f);
    assert(!touch.side_navigation);
    assert(touch.navigation.h == 72.0f);
    assert(touch.navigation.y + touch.navigation.h == 640.0f);
    assert(touch.button_height >= 48.0f);
    assert(touch.content.x == 16.0f);
    assert_valid(touch);

    const auto dense_touch = make_ui_metrics(320.0f, 640.0f, true, 2.0f);
    assert(!dense_touch.side_navigation);
    assert(dense_touch.navigation.h == 144.0f);
    assert(dense_touch.button_height >= 96.0f);
    assert(dense_touch.content.x == 32.0f);
    assert_valid(dense_touch);
    return 0;
}
