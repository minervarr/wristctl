#include <android/configuration.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#include <exception>
#include <memory>
#include <string>

#include "android_host.hh"       // app_shell: the third Host
#include "android_link.hh"       // set the JavaVM/activity for BleLink
#include "arc/fs/paths.hh"       // archive_engine: where the settings live
#include "fullscreen.hh"         // vk_canvas: nav-bar height for the footer
#include "launch_intent.hh"      // app_shell: read a string extra
#include "watch_view.hh"         // the same six pages the desktop draws
#include "wristctl/generated.h"  // core self-report, no JNI codec shim
#include "wristctl/models.h"

#define LOG_TAG "WristctlMain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// AConfiguration's density is the bucketed dp scale Android uses for
// resources (420, 480, ...), not xdpi. That is exactly what a dp layout wants,
// and it needs no Java — which is the point, because this app has no
// AppShellActivity to ask.
float display_density(android_app* state)
{
    AConfiguration* config = AConfiguration_new();
    if (config == nullptr) {
        return 1.0f;
    }
    AConfiguration_fromAssetManager(config, state->activity->assetManager);
    const int32_t dpi = AConfiguration_getDensity(config);
    AConfiguration_delete(config);
    if (dpi <= 0 || dpi == ACONFIGURATION_DENSITY_ANY ||
        dpi == ACONFIGURATION_DENSITY_NONE) {
        return 1.0f;
    }
    return static_cast<float>(dpi) / 160.0f;
}

} // namespace

// The Android sibling of gui/src/gui_main.cc: construct the same WatchView,
// hand it an AndroidHost, run it. When android_main() returns, the activity is
// finished, so every failure is logged with a phase number — there is no
// window left to put an error in.
void android_main(android_app* state)
{
    LOGI("phase 1/5: entry");
    if (state == nullptr || state->activity == nullptr) {
        LOGE("no activity; nothing to run");
        return;
    }

    // Before the worker exists: every make_watch_link() needs the activity to
    // load dev.wristctl.BleLink through the app classloader.
    wristctl::gui::android_link_set_app(state);

    // arc_fs answers where settings go. There is deliberately no shared-storage
    // request in this app: gui.conf is one integer, and internalDataPath is
    // faster and permission-free. Pointing externalRoot at the same private
    // directory keeps the config there too; arc_fs's Android layout otherwise
    // puts it under /storage/emulated/0/home.
    {
        arc::fs::PlatformDirs dirs;
        if (state->activity->internalDataPath != nullptr) {
            dirs.privateDir = state->activity->internalDataPath;
            dirs.externalRoot = dirs.privateDir;
        }
        arc::fs::setPlatformDirs(dirs);
    }

    const float density = display_density(state);
    const bool demo = read_string_extra(state, "demo", "0") == "1";
    const bool auto_connect =
        read_string_extra(state, "autoconnect", "1") == "1";
    LOGI("density %.2fx  demo=%d  autoconnect=%d", density, demo ? 1 : 0,
         auto_connect ? 1 : 0);
    LOGI("core: %u class codecs, %u models, %u flags",
         static_cast<unsigned>(wc_class_count()),
         static_cast<unsigned>(wc_model_count),
         static_cast<unsigned>(wc_flag_count));

    try {
        wristctl::gui::WatchView view(auto_connect && !demo, demo);
        view.set_display_density(density);
        // The navigation bar is visible on this app (the immersive call from
        // app_shell does not stick on this device), so keep the footer above
        // it instead of letting the icon strip cover the hint.
        view.set_system_bottom_inset(
            vce::platform::query_nav_bar_height(state));

        LOGI("phase 2/5: create() -- host init, Vulkan, font");
        if (!view.create(std::make_unique<AndroidHost>(
                state, nullptr, nullptr, /*requestAllFilesAccess=*/false))) {
            LOGE("phase 2/5 FAILED: create() returned false; see the line above");
            return;
        }

        LOGI("phase 3/5: create() OK -- entering run()");
        view.run();
        LOGI("phase 4/5: run() returned -- shutting down");
        view.shutdown();
        LOGI("phase 5/5: clean exit");
    } catch (const std::exception& error) {
        LOGE("FATAL: unhandled exception: %s", error.what());
    } catch (...) {
        LOGE("FATAL: unhandled non-standard exception");
    }
}
