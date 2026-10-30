#pragma once

#include "canvas.hh"

namespace wristctl::gui {

struct UiMetrics {
    Rect navigation;
    Rect header;
    Rect content;
    float content_max_width;
    float margin;
    float spacing;
    float card_radius;
    float title_size;
    float section_size;
    float body_size;
    float secondary_size;
    float hero_size;
    float row_height;
    float button_height;
    float density;
    bool side_navigation;
};

UiMetrics make_ui_metrics(float width, float height, bool touch,
                          float density) noexcept;

} // namespace wristctl::gui
