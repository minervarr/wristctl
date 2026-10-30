#include "ui_theme.hh"

namespace wristctl::gui {

const UiPalette &dark_palette() noexcept
{
    static const UiPalette palette = {
        {0.055f, 0.063f, 0.078f, 1.0f},
        {0.085f, 0.098f, 0.121f, 1.0f},
        {0.115f, 0.133f, 0.161f, 1.0f},
        {0.190f, 0.218f, 0.257f, 1.0f},
        {0.945f, 0.957f, 0.980f, 1.0f},
        {0.680f, 0.722f, 0.780f, 1.0f},
        {0.265f, 0.580f, 0.880f, 1.0f},
        {0.980f, 0.990f, 1.000f, 1.0f},
        {0.300f, 0.790f, 0.550f, 1.0f},
        {0.940f, 0.680f, 0.260f, 1.0f},
        {0.940f, 0.360f, 0.390f, 1.0f},
        {0.390f, 0.430f, 0.490f, 1.0f},
    };
    return palette;
}

} // namespace wristctl::gui
