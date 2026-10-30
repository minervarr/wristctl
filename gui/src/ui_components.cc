#include "ui_components.hh"

#include <algorithm>

namespace wristctl::gui {

void draw_card(Canvas &canvas, const Rect &rect, float radius)
{
    const auto &palette = dark_palette();
    canvas.rect(rect.x, rect.y, rect.w, rect.h, palette.border, radius);
    if (rect.w > 2.0f && rect.h > 2.0f) {
        canvas.rect(rect.x + 1.0f, rect.y + 1.0f, rect.w - 2.0f,
                    rect.h - 2.0f, palette.surface, std::max(0.0f, radius - 1.0f));
    }
}

void draw_button(Canvas &canvas, const Rect &rect, std::string_view label,
                 ButtonVariant variant, float text_size, bool enabled)
{
    const auto &palette = dark_palette();
    Color background = palette.surface_raised;
    Color foreground = palette.text_primary;
    switch (variant) {
    case ButtonVariant::Primary:
        background = palette.accent;
        foreground = palette.accent_text;
        break;
    case ButtonVariant::Secondary:
        background = palette.surface_raised;
        break;
    case ButtonVariant::Quiet:
        background = palette.surface;
        foreground = palette.text_secondary;
        break;
    case ButtonVariant::Destructive:
        background = palette.error;
        foreground = palette.accent_text;
        break;
    }
    if (!enabled) {
        background = palette.surface;
        foreground = palette.disabled;
    }
    canvas.rect(rect.x, rect.y, rect.w, rect.h, background, 10.0f);
    canvas.textCentered(label, rect.x + rect.w * 0.5f,
                        rect.y + (rect.h - text_size) * 0.5f, text_size,
                        foreground);
}

void draw_badge(Canvas &canvas, std::string_view label, float x, float y,
                float text_size, Color color)
{
    const float height = text_size + 12.0f;
    const float width = canvas.textWidth(label, text_size) + 20.0f;
    canvas.rect(x, y, width, height, dark_palette().surface_raised, height * 0.5f);
    canvas.text(label, x + 10.0f, y + 6.0f, text_size, color);
}

void draw_banner(Canvas &canvas, const Rect &rect, std::string_view text,
                 float text_size, Color color)
{
    canvas.rect(rect.x, rect.y, rect.w, rect.h, dark_palette().surface_raised, 10.0f);
    canvas.rect(rect.x, rect.y, 4.0f, rect.h, color, 2.0f);
    canvas.text(text, rect.x + 16.0f, rect.y + (rect.h - text_size) * 0.5f,
                text_size, dark_palette().text_primary);
}

void draw_section_title(Canvas &canvas, std::string_view title, float x, float y,
                        float text_size)
{
    canvas.text(title, x, y, text_size, dark_palette().text_primary);
}

void draw_empty_state(Canvas &canvas, const Rect &rect, std::string_view title,
                      std::string_view body, float title_size, float body_size)
{
    const auto &palette = dark_palette();
    const float center = rect.x + rect.w * 0.5f;
    const float y = rect.y + std::max(0.0f, (rect.h - title_size - body_size - 12.0f) * 0.5f);
    canvas.textCentered(title, center, y, title_size, palette.text_primary);
    canvas.textCentered(body, center, y + title_size + 12.0f, body_size,
                        palette.text_secondary);
}

} // namespace wristctl::gui
