#undef NDEBUG

#include <assert.h>
#include <stdlib.h>

#include "settings.hh"

int main()
{
    setenv("XDG_CONFIG_HOME", "/tmp/opencode/wristctl-settings-test", 1);
    wristctl::gui::save_settings({3});
    assert(wristctl::gui::load_settings().page == 3);
    wristctl::gui::save_settings({5});
    assert(wristctl::gui::load_settings().page == 5);
    /* Out-of-range values in the file fall back to the default. */
    wristctl::gui::save_settings({1});
    assert(wristctl::gui::load_settings().page == 1);
    return 0;
}
