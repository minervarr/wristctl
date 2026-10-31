#include "watch_view.hh"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>

#include "canvas.hh"
#include "host.hh"
#include "keys.hh"
#include "raster_font.hh"
#include "renderer.hh"
#include "settings.hh"
#include "sntp_client.hh"
#include "ui_components.hh"
#include "ui_layout.hh"
#include "ui_theme.hh"
#include "watch_model.hh"

namespace wristctl::gui {
namespace {

constexpr int kPollTimer = 1;

} // namespace

WatchView::WatchView(bool auto_connect, bool demo)
    : worker_(auto_connect, demo), auto_connect_arg_(auto_connect)
{
}
WatchView::~WatchView() = default;

bool WatchView::create()
{
    return create(nullptr);
}

bool WatchView::create(std::unique_ptr<Host> injected_host)
{
    host_ = injected_host ? std::move(injected_host) : make_host();
    if (!host_) {
        std::fprintf(stderr, "error: no window host available\n");
        return false;
    }
    WindowOptions window;
    window.width = 960;
    window.height = 720;
    window.fullscreen = false;
    host_->setWindowOptions(window);
    if (!host_->init(this)) {
        std::fprintf(stderr, "error: could not create the window\n");
        return false;
    }
    touch_ = host_->inputIsTouch();
    try {
        renderer_ = std::make_unique<Renderer>(host_->surfaceProvider(),
                                               host_->assetReader(), 2,
                                               OutputTarget::SdrSrgb,
                                               PresentPolicy::Vsync);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "error: Vulkan initialization failed: %s\n",
                     error.what());
        host_->showErrorMessage("Vulkan initialization failed", error.what());
        return false;
    }

    font_ = std::make_unique<RasterFont>();
    const std::string fontPath = host_->exeDir() + "fonts/ui/ui.otf";
    if (!font_->open(host_->dataReader(), fontPath.c_str())) {
        std::fprintf(stderr,
                     "warning: cannot open %s; using stroke text\n",
                     fontPath.c_str());
        font_.reset();
    } else {
        ensureFont();
    }
    settings_ = load_settings();
    route_ = settings_.route;
    for (const std::string &server : settings_.ntp_servers) {
        if (!ntp_server_text_.empty()) {
            ntp_server_text_ += ' ';
        }
        ntp_server_text_ += server;
    }
    worker_.set_time_config({settings_.use_ntp, settings_.sync_on_connect,
                             settings_.ntp_servers, settings_.ntp_timeout_ms});
    worker_.set_phone_config({settings_.notify_forward, settings_.notify_packages,
                              settings_.notify_quiet_start_min,
                              settings_.notify_quiet_end_min});
    for (const std::string &package : settings_.notify_packages) {
        if (!notify_packages_text_.empty()) {
            notify_packages_text_ += ' ';
        }
        notify_packages_text_ += package;
    }
    snapshot_ = worker_.snapshot();
    seen_version_ = snapshot_.version;
    sync_drafts();
    last_second_ = static_cast<long long>(std::time(nullptr));
    host_->startTimer(kPollTimer, 100);
    host_->showWindow();

    // Deferred until the window exists: the Android permission and
    // Bluetooth-enable dialogs need a resumed activity, and a scan started
    // from the worker's constructor can beat the first window. start() also
    // publishes the platform's scan-list capability for the pairing page.
    // set_auto_connect() applies the saved preference before the first attempt,
    // which is what restores the old "already paired -> connect on launch".
    // --no-connect and the Android demo flag override the SESSION only; the
    // saved preference is not rewritten.
    const bool effective_auto =
        auto_connect_arg_ ? settings_.auto_connect : false;
    worker_.set_auto_connect(effective_auto);
    worker_.start();
    snapshot_ = worker_.snapshot();
    seen_version_ = snapshot_.version;

    dirty_ = true;
    return true;
}

void WatchView::connect_now()
{
    settings_.auto_connect = true;
    save_settings(settings_);
    worker_.set_auto_connect(true);
    worker_.connect();
}

void WatchView::run()
{
    while (running_) {
        beginFrame();
        host_->pump(dirty_);
        if (host_->quitRequested()) {
            running_ = false;
            break;
        }

        // Input edges live only until the next beginFrame(). Draw first when a
        // frame is already pending (a resize or the one-second tick), so the
        // hit boxes describe the layout on screen; then handle this frame's
        // input, and draw again only if an action changed what should be
        // visible. Input arriving with no pending frame is handled against the
        // last drawn layout, which is still the one on screen.
        if (dirty_) {
            drawFrame();
            dirty_ = false;
        }
        handleInput();
        if (dirty_) {
            drawFrame();
            dirty_ = false;
        }
    }
}

void WatchView::onHostResized()
{
    // The Wayland backend does not reliably report a resize through
    // VK_SUBOPTIMAL_KHR when a tiled window moves to another output, so
    // rebuild the swapchain explicitly at the surface's new extent. Without
    // this the renderer keeps drawing at the old size inside the larger
    // window (a 960x720 page in a 1920x1080 tile).
    if (renderer_) {
        renderer_->notifyResized();
    }
    dirty_ = true;
}

void WatchView::onHostLayoutInvalidated()
{
    dirty_ = true;
}

void WatchView::onHostExposed()
{
    dirty_ = true;
}

void WatchView::onAppForegrounded()
{
    // The first start() can race APP_CMD_RESUME (and a permission dialog needs
    // a resumed activity). start() is idempotent; this is the second chance.
    worker_.start();
    dirty_ = true;
}

void WatchView::onTimer(int timerId)
{
    if (timerId != kPollTimer) {
        return;
    }
    const SessionSnapshot latest = worker_.snapshot();
    if (latest.version != seen_version_) {
        seen_version_ = latest.version;
        snapshot_ = latest;
        if (!snapshot_.selected_address.empty()) {
            selected_address_ = snapshot_.selected_address;
        }
        sync_drafts();
        dirty_ = true;
    }
    const long long second = static_cast<long long>(std::time(nullptr));
    if (second != last_second_) {
        last_second_ = second;
        dirty_ = true;
    }
}

void WatchView::onSurfaceLost()
{
    // The surface is going, the device is not. Renderer::recreate_surface()
    // keeps pipelines, textures and the glyph atlas, so there is nothing to
    // tear down here; the fallback rebuild happens in onSurfaceRecreated().
    surface_only_ = true;
}

bool WatchView::onSurfaceRecreated()
{
    if (!renderer_) {
        return true;
    }
    if (surface_only_) {
        surface_only_ = false;
        if (renderer_->recreate_surface()) {
            dirty_ = true;
            return true;
        }
        renderer_.reset();
    }
    try {
        renderer_ = std::make_unique<Renderer>(host_->surfaceProvider(),
                                               host_->assetReader(), 2,
                                               OutputTarget::SdrSrgb,
                                               PresentPolicy::Vsync);
    } catch (const std::exception &error) {
        host_->showErrorMessage("Vulkan re-initialization failed", error.what());
        return false;
    }
    if (font_) {
        renderer_->initMsdf(*font_);
    }
    dirty_ = true;
    return true;
}

void WatchView::onMouseWheel(int x, int y, int delta)
{
    if (editor_open_) {
        return;
    }
    FrameInputView::onMouseWheel(x, y, delta);
    // AndroidHost reports a touch drag in PIXELS; the desktop hosts report
    // Win32's WHEEL_DELTA units (120 per detent). One finger pixel is one
    // content pixel; one wheel detent is a comfortable step.
    const float amount = touch_ ? static_cast<float>(delta)
                                : static_cast<float>(delta) / 120.0f * 40.0f;
    float &scroll = scroll_[route_index(route_)];
    scroll -= amount;
    const float max_scroll =
        std::max(0.0f, static_cast<float>(last_page_height_ - last_viewport_h_));
    scroll = std::clamp(scroll, 0.0f, max_scroll);
    dirty_ = true;
}

void WatchView::shutdown()
{
    running_ = false;
    settings_.route = route_;
    save_settings(settings_);
    worker_.shutdown();
}

void WatchView::clamp_scroll(int page, float page_height, float viewport_h)
{
    const float max_scroll = std::max(0.0f, page_height - viewport_h);
    scroll_[page] = std::clamp(scroll_[page], 0.0f, max_scroll);
}

void WatchView::open_detail(Detail detail, int index)
{
    detail_ = detail;
    detail_index_ = index;
    dirty_ = true;
}

void WatchView::close_detail()
{
    detail_ = Detail::None;
    detail_index_ = 0;
    dirty_ = true;
}

void WatchView::sync_drafts()
{
    if (!alarms_dirty_ && snapshot_.has_alarms) {
        edit_first_ = snapshot_.alarm_first;
        edit_rest_ = snapshot_.alarm_rest;
    }
    if (!timer_dirty_ && snapshot_.has_timer) {
        edit_timer_ = snapshot_.timer_value;
    }
    if (!clock_dirty_ && snapshot_.has_clock) {
        edit_dst_state_ = snapshot_.dst_state;
        memcpy(edit_dst_settings_, snapshot_.dst_settings,
               sizeof edit_dst_settings_);
        memcpy(edit_city_names_, snapshot_.city_names, sizeof edit_city_names_);
        if (!(editor_open_ && editor_field_ == EDIT_CITY_NAME)) {
            edit_city_name_ = edit_city_names_[clock_unit_].name;
        }
    }
    if (!settings_dirty_ && snapshot_.has_settings) {
        edit_settings_ = snapshot_.settings;
    }
    if (!name_dirty_ && !snapshot_.name.empty() &&
        !(editor_open_ && editor_field_ == EDIT_WATCH_NAME)) {
        edit_name_ = snapshot_.name;
    }
    if (notify_app_.empty()) {
        notify_app_ = "wristctl";
    }
}

std::string *WatchView::editor_field()
{
    switch (editor_field_) {
    case EDIT_WATCH_NAME:
        return &edit_name_;
    case EDIT_NOTIFY_APP:
        return &notify_app_;
    case EDIT_NOTIFY_TITLE:
        return &notify_title_;
    case EDIT_NOTIFY_TEXT:
        return &notify_text_;
    case EDIT_NTP_SERVERS:
        return &ntp_server_text_;
    case EDIT_CITY_NAME:
        return &edit_city_name_;
    case EDIT_NOTIFY_PACKAGES:
        return &notify_packages_text_;
    default:
        return nullptr;
    }
}

void WatchView::open_editor(int field)
{
    editor_field_ = field;
    editor_text_.clear();
    if (std::string *value = editor_field()) {
        editor_text_ = *value;
    }
    editor_limit_ = field == EDIT_WATCH_NAME ? 19
                    : field == EDIT_CITY_NAME ? 18
                    : field == EDIT_NOTIFY_PACKAGES ? 1024
                                              : 255;
    if (editor_text_.size() > editor_limit_) {
        editor_text_.resize(editor_limit_);
    }
    editor_open_ = true;
    editor_shift_ = false;
    dirty_ = true;
}

void WatchView::close_editor(bool apply)
{
    const bool fixture_preview =
        snapshot_.demo && demo_scenario_ != DemoScenario::Recorded;
    if (apply) {
        if (std::string *value = editor_field()) {
            *value = editor_text_;
            if (editor_field_ == EDIT_WATCH_NAME) {
                name_dirty_ = true;
            } else if (editor_field_ == EDIT_CITY_NAME) {
                clock_dirty_ = true;
                clock_confirm_ = false;
            } else if (editor_field_ == EDIT_NTP_SERVERS) {
                std::istringstream input(ntp_server_text_);
                std::vector<std::string> servers;
                std::string server;
                while (input >> server) {
                    servers.push_back(server);
                }
                if (!servers.empty()) {
                    settings_.ntp_servers = servers;
                    save_settings(settings_);
                    if (!fixture_preview) {
                        worker_.set_time_config(
                            {settings_.use_ntp, settings_.sync_on_connect,
                             settings_.ntp_servers, settings_.ntp_timeout_ms});
                    }
                }
            } else if (editor_field_ == EDIT_NOTIFY_PACKAGES) {
                std::replace(notify_packages_text_.begin(),
                             notify_packages_text_.end(), ',', ' ');
                std::istringstream input(notify_packages_text_);
                settings_.notify_packages.clear();
                std::string package;
                while (input >> package) {
                    settings_.notify_packages.push_back(package);
                }
                save_settings(settings_);
                if (!fixture_preview) {
                    worker_.set_phone_config(
                        {settings_.notify_forward, settings_.notify_packages,
                         settings_.notify_quiet_start_min,
                         settings_.notify_quiet_end_min});
                }
            }
        }
    }
    editor_open_ = false;
    editor_field_ = EDIT_NONE;
    dirty_ = true;
}

void WatchView::adjust_alarm(int index, int hour_delta, int minute_delta)
{
    wc_alarm_first *first = index == 0 ? &edit_first_ : nullptr;
    wc_alarm_rest_alarms_item *item =
        index == 0 ? nullptr : &edit_rest_.alarms[index - 1];
    int hour = first != nullptr ? first->hour : item->hour;
    int minute = first != nullptr ? first->minute : item->minute;
    minute += minute_delta;
    while (minute < 0) {
        minute += 60;
        hour--;
    }
    while (minute >= 60) {
        minute -= 60;
        hour++;
    }
    hour = (hour + hour_delta + 24) % 24;
    if (first != nullptr) {
        first->hour = static_cast<uint8_t>(hour);
        first->minute = static_cast<uint8_t>(minute);
    } else {
        item->hour = static_cast<uint8_t>(hour);
        item->minute = static_cast<uint8_t>(minute);
    }
    alarms_dirty_ = true;
    alarms_confirm_ = false;
    dirty_ = true;
}

void WatchView::adjust_timer(int index, int field, int delta)
{
    if (index < 0 || index >= 6) {
        return;
    }
    wc_program_timer_workouts_item &item = edit_timer_.workouts[index];
    if (field == 0) {
        int value = static_cast<int>(item.minutes) + delta;
        value = std::clamp(value, 0, 99);
        item.minutes = static_cast<uint8_t>(value);
    } else if (field == 1) {
        int value = static_cast<int>(item.seconds) + delta;
        while (value < 0) {
            if (item.minutes == 0) {
                value = 0;
                break;
            }
            item.minutes--;
            value += 60;
        }
        while (value >= 60) {
            if (item.minutes >= 99) {
                value = 59;
                break;
            }
            item.minutes++;
            value -= 60;
        }
        item.seconds = static_cast<uint8_t>(value);
    } else {
        item.beep = static_cast<uint8_t>((item.beep + delta) & 3);
    }
    timer_dirty_ = true;
    timer_confirm_ = false;
    dirty_ = true;
}

std::string WatchView::truncateToWidth(Canvas &canvas, const std::string &text,
                                       float max_w, float size) const
{
    if (canvas.textWidth(text, size) <= max_w) {
        return text;
    }
    std::string out = text;
    while (!out.empty() &&
           canvas.textWidth(out + "...", size) > max_w) {
        out.pop_back();
    }
    if (out.empty()) {
        return std::string();
    }
    return out + "...";
}

float WatchView::drawWrapped(Canvas &canvas, const std::string &text, float x,
                             float y, float max_w, float size, Color color,
                             float line_h) const
{
    std::string line;
    auto flush = [&]() {
        if (!line.empty()) {
            canvas.text(line, x, y, size, color);
            y += line_h;
            line.clear();
        }
    };

    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\n') {
            flush();
            i++;
            continue;
        }
        size_t j = i;
        while (j < text.size() && text[j] != ' ' && text[j] != '\n') {
            j++;
        }
        const std::string word = text.substr(i, j - i);
        const std::string candidate =
            line.empty() ? word : line + " " + word;
        if (canvas.textWidth(candidate, size) <= max_w) {
            line = candidate;
        } else {
            flush();
            if (canvas.textWidth(word, size) <= max_w) {
                line = word;
            } else {
                // A single token (an address, a UUID) that cannot fit: break
                // it by character so it can never escape the content rect.
                for (char c : word) {
                    std::string next = line;
                    next.push_back(c);
                    if (!line.empty() &&
                        canvas.textWidth(next, size) > max_w) {
                        flush();
                    }
                    line.push_back(c);
                }
            }
        }
        i = j;
        while (i < text.size() && text[i] == ' ') {
            i++;
        }
    }
    flush();
    return y;
}

WatchView::Layout WatchView::make_layout(const Canvas &canvas) const
{
    const UiMetrics metrics = make_ui_metrics(
        canvas.w(), canvas.h(), touch_, display_density_);
    Layout l;
    l.left = canvas.left();
    l.top = canvas.top();
    l.width = canvas.w();
    l.height = canvas.h();
    l.margin = metrics.margin;
    l.header_h = metrics.header.h;
    l.title_size = metrics.title_size;
    l.small_size = metrics.secondary_size;
    l.text_size = metrics.body_size;
    l.page_title_size = metrics.title_size;
    l.clock_size = metrics.hero_size;
    l.date_size = metrics.section_size;
    l.stacked_header = touch_;
    l.action_h = metrics.button_height;
    l.content_x = metrics.content.x;
    l.content_y = metrics.content.y;
    l.content_w = metrics.content.w;
    l.content_bottom = l.top + metrics.content.y + metrics.content.h;
    l.footer_h = 0.0f;
    l.row_line = metrics.row_height;
    l.label_w = std::min(170.0f * metrics.density, l.content_w * 0.42f);
    l.scroll_step = 40.0f * metrics.density;
    return l;
}

void WatchView::addButton(Canvas &canvas, float x, float y, float w, float h,
                          const std::string &label, Action action, int page,
                          bool active, float radius, int arg, int arg2)
{
    canvas.button(x, y, w, h, label,
                  active ? col::accent : col::btnIdle, col::text, radius);
    hits_.push_back({{x, y, w, h}, action, page, {}, arg, arg2});
}

void WatchView::addHit(const Rect &rect, Action action,
                       const std::string &address, int arg, int arg2)
{
    hits_.push_back({rect, action, 0, address, arg, arg2});
}

bool WatchView::selected_is_bonded() const
{
    for (const WatchDevice &device : snapshot_.devices) {
        if (device.address == selected_address_) {
            return device.bonded;
        }
    }
    return false;
}

void WatchView::drawHeader(Canvas &canvas, const Layout &layout)
{
    const auto &palette = dark_palette();
    const UiMetrics metrics = make_ui_metrics(
        layout.width, layout.height, touch_, display_density_);
    const Rect header = {layout.left + metrics.header.x,
                         layout.top + metrics.header.y, metrics.header.w,
                         metrics.header.h};
    canvas.rect(header.x, header.y, header.w, header.h, palette.surface);

    auto utility = [&](const Rect &rect, const std::string &label, Route route) {
        draw_button(canvas, rect, label,
                    route_ == route ? ButtonVariant::Secondary
                                    : ButtonVariant::Quiet,
                    touch_ ? 12.0f * metrics.density : 14.0f);
        hits_.push_back({rect, Action::Page, route_index(route), {}, 0, 0});
    };

    if (touch_) {
        const bool compact = header.w < 500.0f * metrics.density;
        const float utilities_width = compact
            ? header.w - 2.0f * metrics.margin
            : 270.0f * metrics.density;
        if (!compact) {
            canvas.text(std::string(route_name(route_)), header.x + metrics.margin,
                        header.y + (header.h - 20.0f * metrics.density) * 0.5f,
                        20.0f * metrics.density, palette.text_primary);
        }
        const float gap = 6.0f * metrics.density;
        const float width = (utilities_width - 2.0f * gap) / 3.0f;
        const float x = header.x + header.w - metrics.margin - utilities_width;
        const float y = header.y + (header.h - metrics.button_height) * 0.5f;
        utility({x, y, width, metrics.button_height}, "Watch", Route::Watch);
        utility({x + width + gap, y, width, metrics.button_height}, "Settings",
                Route::Settings);
        utility({x + 2.0f * (width + gap), y, width, metrics.button_height},
                "Logs", Route::Diagnostics);
        return;
    }

    canvas.text(std::string(route_name(route_)), header.x + metrics.margin,
                header.y + (header.h - 24.0f) * 0.5f, 24.0f,
                palette.text_primary);
    const Rect connection = {header.x + header.w - metrics.margin - 180.0f,
                             header.y + 12.0f, 180.0f, 40.0f};
    utility(connection, snapshot_.connected ? "Watch connected" : "Connect watch",
            Route::Watch);
}

void WatchView::drawTabs(Canvas &canvas, const Layout &layout)
{
    const auto &palette = dark_palette();
    const UiMetrics metrics = make_ui_metrics(
        layout.width, layout.height, touch_, display_density_);
    const Rect navigation = {layout.left + metrics.navigation.x,
                             layout.top + metrics.navigation.y,
                             metrics.navigation.w, metrics.navigation.h};
    canvas.rect(navigation.x, navigation.y, navigation.w, navigation.h,
                palette.surface);
    constexpr Route primary[] = {Route::Overview, Route::Time, Route::Alarms,
                                 Route::Timer, Route::Phone};
    constexpr const char *labels[] = {"Overview", "Time", "Alarms", "Timer",
                                      "Phone"};

    if (touch_) {
        const float width = navigation.w / 5.0f;
        for (int index = 0; index < 5; ++index) {
            const Rect rect = {navigation.x + index * width, navigation.y + 6.0f,
                               width, navigation.h - 12.0f};
            draw_button(canvas, rect, labels[index],
                        route_ == primary[index] ? ButtonVariant::Secondary
                                                 : ButtonVariant::Quiet,
                        12.0f * metrics.density);
            hits_.push_back(
                {rect, Action::Page, route_index(primary[index]), {}, 0, 0});
        }
        return;
    }

    canvas.text("wristctl", navigation.x + 24.0f, navigation.y + 26.0f, 26.0f,
                palette.text_primary);
    for (int index = 0; index < 5; ++index) {
        const Rect rect = {navigation.x + 16.0f,
                           navigation.y + 92.0f + index * 56.0f,
                           navigation.w - 32.0f, 44.0f};
        draw_button(canvas, rect, labels[index],
                    route_ == primary[index] ? ButtonVariant::Secondary
                                             : ButtonVariant::Quiet,
                    16.0f);
        hits_.push_back(
            {rect, Action::Page, route_index(primary[index]), {}, 0, 0});
    }
    constexpr Route utilities[] = {Route::Watch, Route::Settings,
                                   Route::Diagnostics};
    constexpr const char *utility_labels[] = {"Watch", "Settings", "Diagnostics"};
    for (int index = 0; index < 3; ++index) {
        const Rect rect = {navigation.x + 16.0f,
                           navigation.y + navigation.h - 156.0f + index * 46.0f,
                           navigation.w - 32.0f, 40.0f};
        draw_button(canvas, rect, utility_labels[index],
                    route_ == utilities[index] ? ButtonVariant::Secondary
                                               : ButtonVariant::Quiet,
                    14.0f);
        hits_.push_back(
            {rect, Action::Page, route_index(utilities[index]), {}, 0, 0});
    }
}

void WatchView::draw_editor_key(Canvas &canvas, float x, float y, float w,
                                float h, const std::string &label, int key,
                                Action action)
{
    canvas.button(x, y, w, h, label, col::btnIdle, col::text, h * 0.16f);
    hits_.push_back({{x, y, w, h}, action, 0, {}, key, 0});
}

void WatchView::drawEditor(Canvas &canvas, const Layout &layout)
{
    if (!editor_open_) {
        return;
    }
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float pad = 14 * d;
    const float gap = 5 * d;
    const float panel_x = layout.left + layout.margin;
    const float panel_w = layout.width - 2 * layout.margin;
    const float panel_y = layout.top + layout.header_h + 8 * d;
    const float panel_h = layout.height - layout.header_h - 16 * d;

    canvas.rect(layout.left, layout.top, layout.width, layout.height, col::bg);
    canvas.rect(panel_x, panel_y, panel_w, panel_h, col::panel, 10 * d);

    const char *title = "Edit";
    switch (editor_field_) {
    case EDIT_WATCH_NAME:
        title = "Watch name (19 bytes)";
        break;
    case EDIT_NOTIFY_APP:
        title = "Notification app";
        break;
    case EDIT_NOTIFY_TITLE:
        title = "Notification title";
        break;
    case EDIT_NOTIFY_TEXT:
        title = "Notification text";
        break;
    case EDIT_NTP_SERVERS:
        title = "SNTP servers (space separated)";
        break;
    case EDIT_CITY_NAME:
        title = "City label (18 bytes)";
        break;
    case EDIT_NOTIFY_PACKAGES:
        title = "Allowed packages (space/comma separated)";
        break;
    default:
        break;
    }
    canvas.text(title, panel_x + pad, panel_y + pad + layout.text_size * 0.8f,
                layout.text_size, col::text);

    const float text_y = panel_y + pad + layout.text_size * 2.0f;
    float text_w = panel_w - 2 * pad;
    std::string shown = editor_text_ + "_";
    while (!shown.empty() && canvas.textWidth(shown, layout.text_size) >
                                 text_w) {
        shown.erase(0, 1);
    }
    canvas.rect(panel_x + pad, text_y - 6 * d, text_w,
                layout.text_size * 1.9f, col::panel2, 5 * d);
    canvas.text(shown, panel_x + pad + 8 * d, text_y + layout.text_size,
                layout.text_size, col::text);

    static const char *const rows[] = {"1234567890", "qwertyuiop", "asdfghjkl",
                                       "zxcvbnm", ".-_"};
    const float keys_top = text_y + layout.text_size * 2.8f;
    const float available = panel_y + panel_h - pad - keys_top;
    float key_h = (available - gap * 5) / 6.0f;
    key_h = std::clamp(key_h, 26 * d, 46 * d);
    const float key_gap = gap;
    const float key_w = (panel_w - 2 * pad - key_gap * 9) / 10;
    float y = keys_top;
    for (const char *row : rows) {
        float x = panel_x + pad;
        for (const char *p = row; *p != '\0'; p++) {
            draw_editor_key(canvas, x, y, key_w, key_h, std::string(1, *p),
                            *p, Action::EditorKey);
            x += key_w + key_gap;
        }
        y += key_h + key_gap;
    }

    const float special_w = (panel_w - 2 * pad - key_gap * 3) / 4;
    float x = panel_x + pad;
    draw_editor_key(canvas, x, y, special_w, key_h, "Space", ' ',
                    Action::EditorSpace);
    x += special_w + key_gap;
    draw_editor_key(canvas, x, y, special_w, key_h, "Del", 0,
                    Action::EditorBackspace);
    x += special_w + key_gap;
    draw_editor_key(canvas, x, y, special_w, key_h, "Cancel", 0,
                    Action::EditorCancel);
    x += special_w + key_gap;
    draw_editor_key(canvas, x, y, special_w, key_h, "Done", 0,
                    Action::EditorDone);
}

void WatchView::drawFooter(Canvas &canvas, const Layout &layout)
{
    const float y = layout.top + layout.height - layout.footer_h;
    canvas.rect(layout.left, y, layout.width, layout.footer_h, col::panel);
    const std::string hint =
        touch_ ? "drag to scroll    tap a tab"
               : "1-5 pages  C connect/auto  R refresh  T sync  Esc quit";
    canvas.text(hint, layout.left + layout.margin, y + layout.footer_h * 0.22f,
                layout.small_size, col::dim);
}

void WatchView::handleClick(float x, float y)
{
    if (editor_open_) {
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            if (!it->rect.contains(x, y)) {
                continue;
            }
            switch (it->action) {
            case Action::EditorKey:
                if (editor_text_.size() < editor_limit_) {
                    editor_text_.push_back(static_cast<char>(it->arg));
                }
                break;
            case Action::EditorSpace:
                if (editor_text_.size() < editor_limit_) {
                    editor_text_.push_back(' ');
                }
                break;
            case Action::EditorBackspace:
                if (!editor_text_.empty()) {
                    editor_text_.pop_back();
                    while (!editor_text_.empty() &&
                           (static_cast<unsigned char>(editor_text_.back()) &
                            0xC0) == 0x80) {
                        editor_text_.pop_back();
                    }
                }
                break;
            case Action::EditorDone:
                close_editor(true);
                break;
            case Action::EditorCancel:
                close_editor(false);
                break;
            default:
                continue;
            }
            dirty_ = true;
            return;
        }
        return;
    }

    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (!it->rect.contains(x, y)) {
            continue;
        }
        if (snapshot_.demo && demo_scenario_ != DemoScenario::Recorded) {
            switch (it->action) {
            case Action::Page:
            case Action::DemoScenario:
            case Action::SelectDevice:
            case Action::ToggleAll:
            case Action::AlarmEdit:
            case Action::AlarmToggle:
            case Action::AlarmMode:
            case Action::AlarmChime:
            case Action::AlarmHour:
            case Action::AlarmMinute:
            case Action::AlarmRevert:
            case Action::TimerEdit:
            case Action::TimerRepeat:
            case Action::TimerMinutes:
            case Action::TimerSeconds:
            case Action::TimerBeep:
            case Action::TimerInitialize:
            case Action::TimerRevert:
            case Action::ClockUnit:
            case Action::CloseDetail:
            case Action::CityTimezone:
            case Action::CityDstDiff:
            case Action::CityDstToggle:
            case Action::CityRevert:
            case Action::ToggleHomeCard:
            case Action::ToggleAppTimeFormat:
            case Action::SettingToggle:
            case Action::SettingCycle:
            case Action::SettingsRevert:
            case Action::EditField:
            case Action::NotificationTest:
            case Action::NotifyType:
            case Action::EditorKey:
            case Action::EditorSpace:
            case Action::EditorBackspace:
            case Action::EditorDone:
            case Action::EditorCancel:
                break;
            default:
                return;
            }
        }
        switch (it->action) {
        case Action::DemoScenario:
            demo_scenario_ = static_cast<DemoScenario>(it->arg);
            if (demo_scenario_ == DemoScenario::Recorded) {
                snapshot_ = worker_.snapshot();
                alarms_dirty_ = false;
                alarms_confirm_ = false;
                timer_dirty_ = false;
                timer_confirm_ = false;
                clock_dirty_ = false;
                clock_confirm_ = false;
                settings_dirty_ = false;
                name_dirty_ = false;
                sync_drafts();
            }
            break;
        case Action::Connect:
            worker_.connect();
            break;
        case Action::Scan:
            selected_address_.clear();
            route_ = Route::Watch;
            settings_.route = route_;
            save_settings(settings_);
            worker_.scan();
            break;
        case Action::StopScan:
            worker_.stop_scan();
            break;
        case Action::ConnectSelected:
            if (!selected_address_.empty()) {
                worker_.connect_to(selected_address_);
            }
            break;
        case Action::SelectDevice:
            selected_address_ = it->address;
            break;
        case Action::ToggleAll:
            show_all_ = !show_all_;
            break;
        case Action::Grant:
            worker_.grant_permissions();
            break;
        case Action::OpenSettings:
            worker_.open_settings();
            break;
        case Action::TurnOnBluetooth:
            worker_.enable_bluetooth();
            break;
        case Action::OpenLocation:
            worker_.open_location_settings();
            break;
        case Action::ToggleAutoConnect: {
            const bool next = !snapshot_.auto_connect;
            settings_.auto_connect = next;
            save_settings(settings_);
            worker_.set_auto_connect(next);
            break;
        }
        case Action::Disconnect:
            settings_.auto_connect = false;
            save_settings(settings_);
            worker_.disconnect();
            break;
        case Action::Refresh:
            worker_.refresh();
            break;
        case Action::SyncTime:
            worker_.sync_time();
            break;
        case Action::ToggleTimeSource:
            settings_.use_ntp = !settings_.use_ntp;
            save_settings(settings_);
            worker_.set_time_config(
                {settings_.use_ntp, settings_.sync_on_connect,
                 settings_.ntp_servers, settings_.ntp_timeout_ms});
            break;
        case Action::ToggleSyncOnConnect:
            settings_.sync_on_connect = !settings_.sync_on_connect;
            save_settings(settings_);
            worker_.set_time_config(
                {settings_.use_ntp, settings_.sync_on_connect,
                 settings_.ntp_servers, settings_.ntp_timeout_ms});
            break;
        case Action::ToggleAppTimeFormat:
            settings_.app_time_24h = !settings_.app_time_24h;
            save_settings(settings_);
            break;
        case Action::ToggleHomeCard: {
            bool *visible = nullptr;
            switch (it->arg) {
            case 0:
                visible = &settings_.home_show_connection;
                break;
            case 1:
                visible = &settings_.home_show_clock;
                break;
            case 2:
                visible = &settings_.home_show_alarms;
                break;
            case 3:
                visible = &settings_.home_show_timer;
                break;
            default:
                break;
            }
            if (visible != nullptr) {
                *visible = !*visible;
                save_settings(settings_);
            }
            break;
        }
        case Action::Page:
            if (it->page >= 0 && it->page < kRouteCount) {
                route_ = static_cast<Route>(it->page);
                settings_.route = route_;
                save_settings(settings_);
            }
            break;
        case Action::AlarmEdit:
            if (it->arg >= 0 && it->arg < 5) {
                open_detail(Detail::Alarm, it->arg);
            }
            break;
        case Action::AlarmToggle:
            if (it->arg == 0) {
                edit_first_.enabled = !edit_first_.enabled;
            } else {
                auto &alarm = edit_rest_.alarms[it->arg - 1];
                alarm.enabled = !alarm.enabled;
            }
            alarms_dirty_ = true;
            alarms_confirm_ = false;
            break;
        case Action::AlarmMode:
            if (it->arg == 0) {
                edit_first_.one_time = !edit_first_.one_time;
            } else {
                auto &alarm = edit_rest_.alarms[it->arg - 1];
                alarm.one_time = !alarm.one_time;
            }
            alarms_dirty_ = true;
            alarms_confirm_ = false;
            break;
        case Action::AlarmChime:
            edit_first_.hourly_chime = !edit_first_.hourly_chime;
            alarms_dirty_ = true;
            alarms_confirm_ = false;
            break;
        case Action::AlarmHour:
            adjust_alarm(it->arg, it->arg2, 0);
            break;
        case Action::AlarmMinute:
            adjust_alarm(it->arg, 0, it->arg2);
            break;
        case Action::AlarmApply:
            if (alarms_dirty_) {
                if (!alarms_confirm_) {
                    alarms_confirm_ = true;
                } else {
                    worker_.write_alarms(edit_first_, edit_rest_);
                    alarms_dirty_ = false;
                    alarms_confirm_ = false;
                }
            }
            break;
        case Action::AlarmRevert:
            if (snapshot_.has_alarms) {
                edit_first_ = snapshot_.alarm_first;
                edit_rest_ = snapshot_.alarm_rest;
            }
            alarms_dirty_ = false;
            alarms_confirm_ = false;
            break;
        case Action::TimerEdit:
            if (it->arg >= 0 && it->arg < 6) {
                open_detail(Detail::TimerStep, it->arg);
            }
            break;
        case Action::TimerRepeat:
            edit_timer_.repeat = static_cast<uint8_t>(std::clamp(
                static_cast<int>(edit_timer_.repeat) + it->arg, 0, 99));
            timer_dirty_ = true;
            timer_confirm_ = false;
            break;
        case Action::TimerMinutes:
            adjust_timer(it->arg, 0, it->arg2);
            break;
        case Action::TimerSeconds:
            adjust_timer(it->arg, 1, it->arg2);
            break;
        case Action::TimerBeep:
            adjust_timer(it->arg, 2, it->arg2);
            break;
        case Action::TimerInitialize:
            memset(&edit_timer_, 0, sizeof edit_timer_);
            timer_dirty_ = true;
            timer_confirm_ = false;
            break;
        case Action::TimerApply:
            if (timer_dirty_) {
                if (!timer_confirm_) {
                    timer_confirm_ = true;
                } else {
                    worker_.write_timer(edit_timer_);
                    timer_dirty_ = false;
                    timer_confirm_ = false;
                }
            }
            break;
        case Action::TimerRevert:
            if (snapshot_.has_timer) {
                edit_timer_ = snapshot_.timer_value;
            }
            timer_dirty_ = false;
            timer_confirm_ = false;
            break;
        case Action::ClockUnit:
            if ((!clock_dirty_ || it->arg == clock_unit_) && it->arg >= 0 &&
                it->arg < 5) {
                if (!clock_dirty_) {
                    clock_unit_ = it->arg;
                    edit_city_name_ = edit_city_names_[clock_unit_].name;
                    clock_confirm_ = false;
                }
                open_detail(Detail::City, clock_unit_);
            }
            break;
        case Action::CloseDetail:
            close_detail();
            break;
        case Action::CityTimezone:
            edit_dst_settings_[clock_unit_].timezone = static_cast<int8_t>(
                std::clamp(static_cast<int>(
                               edit_dst_settings_[clock_unit_].timezone) +
                               it->arg,
                           -56, 56));
            clock_dirty_ = true;
            clock_confirm_ = false;
            break;
        case Action::CityDstDiff:
            edit_dst_settings_[clock_unit_].dst_diff = static_cast<uint8_t>(
                std::clamp(static_cast<int>(
                               edit_dst_settings_[clock_unit_].dst_diff) +
                               it->arg,
                           0, 8));
            clock_dirty_ = true;
            clock_confirm_ = false;
            break;
        case Action::CityDstToggle:
            if (clock_unit_ == 0) {
                if (it->arg == 0) {
                    edit_dst_state_.dst_on_home = !edit_dst_state_.dst_on_home;
                } else {
                    edit_dst_state_.dst_auto_home =
                        !edit_dst_state_.dst_auto_home;
                }
            } else if (clock_unit_ == 1) {
                if (it->arg == 0) {
                    edit_dst_state_.dst_on_world = !edit_dst_state_.dst_on_world;
                } else {
                    edit_dst_state_.dst_auto_world =
                        !edit_dst_state_.dst_auto_world;
                }
            }
            clock_dirty_ = true;
            clock_confirm_ = false;
            break;
        case Action::CityApply:
            if (clock_dirty_ && snapshot_.has_clock) {
                if (!clock_confirm_) {
                    clock_confirm_ = true;
                    break;
                }
                wc_city_name &name = edit_city_names_[clock_unit_];
                memset(name.name, 0, sizeof name.name);
                memcpy(name.name, edit_city_name_.c_str(),
                       std::min(edit_city_name_.size(), sizeof name.name - 1));
                const uint16_t city_id = edit_dst_settings_[clock_unit_].city;
                switch (clock_unit_) {
                case 0:
                    edit_dst_state_.city_home = city_id;
                    break;
                case 1:
                    edit_dst_state_.city_world = city_id;
                    break;
                case 2:
                    edit_dst_state_.city_extra_1 = city_id;
                    break;
                case 3:
                    edit_dst_state_.city_extra_2 = city_id;
                    break;
                case 4:
                    edit_dst_state_.city_extra_3 = city_id;
                    break;
                default:
                    break;
                }
                worker_.write_city(static_cast<uint8_t>(clock_unit_),
                                   edit_dst_state_,
                                   edit_dst_settings_[clock_unit_], name);
                clock_dirty_ = false;
                clock_confirm_ = false;
            }
            break;
        case Action::CityRevert:
            if (snapshot_.has_clock) {
                edit_dst_state_ = snapshot_.dst_state;
                memcpy(edit_dst_settings_, snapshot_.dst_settings,
                       sizeof edit_dst_settings_);
                memcpy(edit_city_names_, snapshot_.city_names,
                       sizeof edit_city_names_);
                edit_city_name_ = edit_city_names_[clock_unit_].name;
            }
            clock_dirty_ = false;
            clock_confirm_ = false;
            break;
        case Action::SettingToggle: {
            bool next = false;
            switch (it->arg) {
            case 0:
                next = edit_settings_.time_format_24h == 0;
                edit_settings_.time_format_24h = next ? 1 : 0;
                break;
            case 1:
                next = edit_settings_.button_tone_off != 0;
                edit_settings_.button_tone_off = next ? 0 : 1;
                break;
            case 2:
                next = edit_settings_.auto_light_off != 0;
                edit_settings_.auto_light_off = next ? 0 : 1;
                break;
            case 3:
                next = edit_settings_.power_saving_off != 0;
                edit_settings_.power_saving_off = next ? 0 : 1;
                break;
            default:
                break;
            }
            settings_dirty_ = true;
            break;
        }
        case Action::SettingCycle:
            switch (it->arg) {
            case 4:
                edit_settings_.light_duration =
                    edit_settings_.light_duration == 0 ? 1 : 0;
                break;
            case 5:
                edit_settings_.date_format_ddmm =
                    edit_settings_.date_format_ddmm == 0 ? 1 : 0;
                break;
            case 6:
                edit_settings_.language =
                    static_cast<uint8_t>((edit_settings_.language + 1) % 6);
                break;
            default:
                break;
            }
            settings_dirty_ = true;
            break;
        case Action::SettingsApply:
            if (settings_dirty_ || name_dirty_) {
                if (name_dirty_) {
                    worker_.write_settings(edit_settings_, edit_name_);
                } else {
                    worker_.write_settings(edit_settings_);
                }
                settings_dirty_ = false;
                name_dirty_ = false;
            }
            break;
        case Action::SettingsRevert:
            if (snapshot_.has_settings) {
                edit_settings_ = snapshot_.settings;
            }
            edit_name_ = snapshot_.name;
            settings_dirty_ = false;
            name_dirty_ = false;
            break;
        case Action::EditField:
            open_editor(it->arg);
            break;
        case Action::ToggleNotifyForward:
            settings_.notify_forward = !settings_.notify_forward;
            save_settings(settings_);
            worker_.set_phone_config(
                {settings_.notify_forward, settings_.notify_packages,
                 settings_.notify_quiet_start_min,
                 settings_.notify_quiet_end_min});
            break;
        case Action::OpenNotificationAccess:
            worker_.open_notification_settings();
            break;
        case Action::NotifyQuietHours:
            if (settings_.notify_quiet_start_min < 0 ||
                settings_.notify_quiet_end_min < 0) {
                settings_.notify_quiet_start_min = 22 * 60;
                settings_.notify_quiet_end_min = 7 * 60;
            } else if (settings_.notify_quiet_start_min == 22 * 60 &&
                       settings_.notify_quiet_end_min == 7 * 60) {
                settings_.notify_quiet_start_min = 0;
                settings_.notify_quiet_end_min = 6 * 60;
            } else {
                settings_.notify_quiet_start_min = -1;
                settings_.notify_quiet_end_min = -1;
            }
            save_settings(settings_);
            worker_.set_phone_config(
                {settings_.notify_forward, settings_.notify_packages,
                 settings_.notify_quiet_start_min,
                 settings_.notify_quiet_end_min});
            break;
        case Action::StopPhoneFinder:
            worker_.stop_phone_finder();
            break;
        case Action::NotificationTest:
            open_detail(Detail::NotificationTest);
            break;
        case Action::NotifyType:
            notify_type_ = (notify_type_ + 1) & 3;
            break;
        case Action::NotifySend:
            worker_.send_notification(static_cast<uint8_t>(notify_type_),
                                      notify_app_, notify_title_,
                                      notify_text_);
            break;
        case Action::EditorKey:
        case Action::EditorSpace:
        case Action::EditorBackspace:
        case Action::EditorDone:
        case Action::EditorCancel:
            break;
        }
        dirty_ = true;
        return;
    }
}

void WatchView::ensureFont()
{
    if (!font_ || !renderer_) {
        return;
    }
    std::vector<uint32_t> codepoints;
    for (uint32_t cp = 32; cp < 127; cp++) {
        codepoints.push_back(cp);
    }
    std::vector<int> sizes;
    if (touch_) {
        const float d = std::max(1.0f, display_density_);
        // Every dp role the touch layout can ask for, plus the button label
        // sizes (Canvas::button uses 0.32 * height).
        const float roles[] = {10, 11, 12, 13, 14, 15, 16, 18, 20,
                               22, 24, 26, 28, 32, 36, 44, 48, 52,
                               56, 64};
        for (float role : roles) {
            sizes.push_back(static_cast<int>(std::lround(role * d)));
        }
    } else {
        sizes = {10, 11, 12, 14, 15, 16, 18, 20, 22, 24, 26, 28, 32, 36, 86};
    }
    std::sort(sizes.begin(), sizes.end());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    const int added = font_->ensureGlyphs(codepoints, sizes);
    if (added > 0 || !renderer_->msdfReady()) {
        renderer_->initMsdf(*font_);
    }
}

void WatchView::drawFrame()
{
    const uint32_t width = renderer_->width();
    const uint32_t height = renderer_->height();
    width_ = static_cast<int>(width);
    height_ = static_cast<int>(height);

    const SessionSnapshot latest = worker_.snapshot();
    if (latest.version != seen_version_) {
        seen_version_ = latest.version;
        snapshot_ = latest;
        if (!snapshot_.selected_address.empty()) {
            selected_address_ = snapshot_.selected_address;
        }
        sync_drafts();
    }
    if (snapshot_.demo && demo_scenario_ != DemoScenario::Recorded) {
        snapshot_ = make_demo_snapshot(latest, demo_scenario_);
        const bool preview_dirty = demo_scenario_ == DemoScenario::Dirty ||
                                   demo_scenario_ == DemoScenario::Confirmation;
        const bool preview_confirmation =
            demo_scenario_ == DemoScenario::Confirmation;
        alarms_dirty_ = preview_dirty;
        alarms_confirm_ = preview_confirmation;
        timer_dirty_ = preview_dirty;
        timer_confirm_ = preview_confirmation;
        clock_dirty_ = preview_dirty;
        clock_confirm_ = preview_confirmation;
    }

    curves_.clear();
    shapes_.clear();
    textQuads_.clear();
    hits_.clear();

    if (font_ && font_->hasMisses()) {
        if (font_->bakeMisses() > 0) {
            renderer_->initMsdf(*font_);
        }
    }

    const SafeInsets insets = host_->safeInsets();
    Canvas canvas(curves_, width, height, nullptr,
                  static_cast<float>(insets.top),
                  static_cast<float>(insets.bottom + system_bottom_inset_),
                  static_cast<float>(insets.left),
                  static_cast<float>(insets.right));
    canvas.useShapes(&shapes_);
    if (font_) {
        canvas.useMsdf(font_.get(), &textQuads_);
    }
    canvas.clear(col::bg);

    const Layout layout = make_layout(canvas);
    if (editor_open_) {
        drawEditor(canvas, layout);
        renderer_->draw(curves_, 0, {}, {}, textQuads_, shapes_);
        return;
    }

    const float clip_y = layout.top + layout.content_y;
    const float clip_h = std::max(0.0f, layout.content_bottom - clip_y);
    clamp_scroll(route_index(route_), static_cast<float>(last_page_height_),
                 clip_h);

    canvas.setClip(layout.left + layout.content_x, clip_y, layout.content_w,
                   clip_h);
    float page_height = 0;
    switch (route_) {
    case Route::Overview:
        page_height = drawOverviewPage(canvas, layout);
        break;
    case Route::Watch:
        page_height = drawWatchPage(canvas, layout);
        break;
    case Route::Time:
        page_height = detail_ == Detail::City ? drawCityEditor(canvas, layout)
                                              : drawTimePage(canvas, layout);
        break;
    case Route::Alarms:
        page_height = detail_ == Detail::Alarm ? drawAlarmEditor(canvas, layout)
                                               : drawAlarmsPage(canvas, layout);
        break;
    case Route::Timer:
        page_height = detail_ == Detail::TimerStep
            ? drawTimerStepEditor(canvas, layout)
            : drawTimerPage(canvas, layout);
        break;
    case Route::Phone:
        page_height = detail_ == Detail::NotificationTest
            ? drawNotificationTest(canvas, layout)
            : drawNotifyPage(canvas, layout);
        break;
    case Route::Settings:
        page_height = drawSettingsPage(canvas, layout);
        break;
    case Route::Diagnostics:
        page_height = drawDiagnosticsPage(canvas, layout);
        break;
    case Route::Count:
        break;
    }
    canvas.clearClip();
    last_page_height_ = static_cast<int>(page_height);
    last_viewport_h_ = static_cast<int>(clip_h);

    // Chrome is drawn AFTER the content so the clip's tile-granular bleed can
    // never paint over a header or footer.
    drawHeader(canvas, layout);
    drawTabs(canvas, layout);
    drawFooter(canvas, layout);
    drawEditor(canvas, layout);

    renderer_->draw(curves_, 0, {}, {}, textQuads_, shapes_);
}

void WatchView::handleInput()
{
    if (input().pointerWentDown) {
        handleClick(input().pointerX, input().pointerY);
    }
    if (editor_open_) {
        if (input().keyWentDown(key::Escape)) {
            close_editor(false);
        }
        return;
    }
    constexpr Route primary[] = {Route::Overview, Route::Time, Route::Alarms,
                                 Route::Timer, Route::Phone};
    for (int key = 0; key < 5; key++) {
        if (input().keyWentDown(0x31 + key)) {
            route_ = primary[key];
            settings_.route = route_;
            save_settings(settings_);
            dirty_ = true;
        }
    }
    const bool fixture_preview =
        snapshot_.demo && demo_scenario_ != DemoScenario::Recorded;
    if (!fixture_preview && input().keyWentDown(0x43)) { // C
        settings_.auto_connect = true;
        save_settings(settings_);
        worker_.set_auto_connect(true);
        worker_.connect();
        dirty_ = true;
    }
    if (!fixture_preview && input().keyWentDown(0x52)) { // R
        worker_.refresh();
        dirty_ = true;
    }
    if (!fixture_preview && input().keyWentDown(0x54)) { // T
        worker_.sync_time();
        dirty_ = true;
    }
    if (input().keyWentDown(key::Escape)) {
        if (detail_ != Detail::None) {
            close_detail();
        } else {
            running_ = false;
        }
    }
}

} // namespace wristctl::gui
