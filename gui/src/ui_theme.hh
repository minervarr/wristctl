#pragma once

#include "canvas.hh"

namespace wristctl::gui {

struct UiPalette {
    Color canvas;
    Color surface;
    Color surface_raised;
    Color border;
    Color text_primary;
    Color text_secondary;
    Color accent;
    Color accent_text;
    Color success;
    Color warning;
    Color error;
    Color disabled;
};

const UiPalette &dark_palette() noexcept;

} // namespace wristctl::gui
