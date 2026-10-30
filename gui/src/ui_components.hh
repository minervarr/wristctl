#pragma once

#include <string_view>

#include "ui_theme.hh"

namespace wristctl::gui {

enum class ButtonVariant { Primary, Secondary, Quiet, Destructive };

void draw_card(Canvas &canvas, const Rect &rect, float radius = 12.0f);
void draw_button(Canvas &canvas, const Rect &rect, std::string_view label,
                 ButtonVariant variant, float text_size, bool enabled = true);
void draw_badge(Canvas &canvas, std::string_view label, float x, float y,
                float text_size, Color color);
void draw_banner(Canvas &canvas, const Rect &rect, std::string_view text,
                 float text_size, Color color);
void draw_section_title(Canvas &canvas, std::string_view title, float x, float y,
                        float text_size);
void draw_empty_state(Canvas &canvas, const Rect &rect, std::string_view title,
                      std::string_view body, float title_size, float body_size);

} // namespace wristctl::gui
