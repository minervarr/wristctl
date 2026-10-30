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
#include "watch_model.hh"

namespace wristctl::gui {
namespace {

constexpr int kPollTimer = 1;
constexpr float kText = 18.0f;
constexpr float kSmall = 14.0f;
constexpr float kTitle = 28.0f;

const char *const kPageNames[] = {
    "Home", "Watch", "Clock", "Alarms", "Timer",
    "Training", "Activity", "Phone", "Settings", "Console"};

Color statusColor(const SessionSnapshot &snapshot)
{
    if (!snapshot.error.empty()) {
        return col::red;
    }
    if (snapshot.busy) {
        return col::yellow;
    }
    return snapshot.connected ? col::green : col::dim;
}

std::string status_line(const SessionSnapshot &snapshot)
{
    return snapshot.status +
           (snapshot.auto_connect ? std::string() : std::string("  [auto off]"));
}

bool looks_like_watch_name(const std::string &name)
{
    static const char *const prefixes[] = {
        "CASIO", "GBX", "GBD", "GPR", "GWF", "GST", "GMW", "MRG", "DW-", "GM-"
    };
    std::string upper;
    upper.reserve(name.size());
    for (char c : name) {
        upper.push_back(static_cast<char>(std::toupper(
            static_cast<unsigned char>(c))));
    }
    for (const char *prefix : prefixes) {
        if (upper.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

std::string frame_text(const wc_frame &frame)
{
    if (frame.len == 0) {
        return "not read";
    }
    char bytes[WC_FRAME_MAX * 3 + 1]{};
    size_t used = 0;
    for (uint8_t index = 0; index < frame.len && index < WC_FRAME_MAX; index++) {
        used += static_cast<size_t>(std::snprintf(
            bytes + used, sizeof bytes - used, "%s%02x", index == 0 ? "" : " ",
            frame.data[index]));
    }
    return bytes;
}

std::string offset_text(int quarters, bool utc)
{
    const int minutes = quarters * 15;
    const int magnitude = minutes < 0 ? -minutes : minutes;
    char text[32];
    std::snprintf(text, sizeof text, "%s%s%02d:%02d", utc ? "UTC" : "",
                  minutes < 0 ? "-" : "+", magnitude / 60, magnitude % 60);
    return text;
}

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
    page_ = route_index(settings_.route);
    if (page_ >= 5) {
        page_ += 2;
    }
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
    scroll_[page_] -= amount;
    const float max_scroll =
        std::max(0.0f, static_cast<float>(last_page_height_ - last_viewport_h_));
    scroll_[page_] = std::clamp(scroll_[page_], 0.0f, max_scroll);
    dirty_ = true;
}

void WatchView::shutdown()
{
    running_ = false;
    settings_.route = route_from_legacy_page(page_);
    save_settings(settings_);
    worker_.shutdown();
}

void WatchView::clamp_scroll(int page, float page_height, float viewport_h)
{
    const float max_scroll = std::max(0.0f, page_height - viewport_h);
    scroll_[page] = std::clamp(scroll_[page], 0.0f, max_scroll);
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
                    worker_.set_time_config(
                        {settings_.use_ntp, settings_.sync_on_connect,
                         settings_.ntp_servers, settings_.ntp_timeout_ms});
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
                worker_.set_phone_config(
                    {settings_.notify_forward, settings_.notify_packages,
                     settings_.notify_quiet_start_min,
                     settings_.notify_quiet_end_min});
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
    Layout l;
    l.left = canvas.left();
    l.top = canvas.top();
    l.width = canvas.w();
    l.height = canvas.h();

    if (!touch_) {
        l.margin = 20;
        l.header_h = 62;
        l.title_size = kTitle;
        l.small_size = kSmall;
        l.text_size = kText;
        l.page_title_size = 26;
        l.clock_size = 86;
        l.date_size = 22;
        l.title_y = 12;
        l.status_y = 20;
        l.detail_y = 40;
        l.stacked_header = false;
        l.tabs_y = 74;
        l.tab_h = 34;
        l.tab_gap = 6;
        l.tab_cols = 5;
        l.tab_rows = (kPageCount + l.tab_cols - 1) / l.tab_cols;
        l.tab_w = (l.width - 2 * l.margin - l.tab_gap * 4) / 5;
        l.actions_y = l.tabs_y + l.tab_rows * (l.tab_h + l.tab_gap) + 8;
        l.action_h = 32;
        l.content_x = 30;
        l.content_y = l.actions_y + l.action_h + 12;
        l.content_w = l.width - 60;
        l.footer_h = 34;
        l.row_line = 28;
        l.label_w = 150;
        l.scroll_step = 40;
    } else {
        const float d = std::max(1.0f, display_density_);
        l.margin = 16 * d;
        l.header_h = 64 * d;
        l.title_size = 20 * d;
        l.small_size = 12 * d;
        l.text_size = 15 * d;
        l.page_title_size = 20 * d;
        l.clock_size = 56 * d;
        l.date_size = 16 * d;
        l.title_y = 8 * d;
        l.status_y = 30 * d;
        l.detail_y = 47 * d;
        l.stacked_header = true;

        l.tabs_y = l.header_h + 8 * d;
        l.tab_h = 48 * d;
        l.tab_gap = 8 * d;
        const float min_tab_w = 105 * d;
        int cols = static_cast<int>(
            (l.width - 2 * l.margin + l.tab_gap) / (min_tab_w + l.tab_gap));
        cols = std::clamp(cols, 1, 5);
        l.tab_cols = cols;
        l.tab_rows = (kPageCount + cols - 1) / cols;
        l.tab_w = (l.width - 2 * l.margin -
                   l.tab_gap * (cols - 1)) / cols;
        l.actions_y = l.tabs_y + l.tab_rows * (l.tab_h + l.tab_gap) + 8 * d;
        l.action_h = 48 * d;
        l.content_x = l.margin;
        l.content_y = l.actions_y + l.action_h + 12 * d;
        l.content_w = l.width - 2 * l.margin;
        l.footer_h = 32 * d;
        l.row_line = 30 * d;
        l.label_w = std::min(130 * d, l.content_w * 0.42f);
        l.scroll_step = 30 * d;
    }
    l.content_bottom = l.top + l.height - l.footer_h;
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
    canvas.rect(layout.left, layout.top, layout.width, layout.header_h,
                col::panel);
    const float right = layout.left + layout.width - layout.margin;

    if (!layout.stacked_header) {
        canvas.text("wristctl", layout.left + 20, layout.top + layout.title_y,
                    layout.title_size, col::text);
        canvas.textRight(status_line(snapshot_), right,
                         layout.top + layout.status_y, layout.small_size,
                         statusColor(snapshot_));
        if (!snapshot_.error.empty()) {
            canvas.textRight(snapshot_.error, right,
                             layout.top + layout.detail_y, layout.small_size,
                             col::red);
        } else if (snapshot_.connected) {
            canvas.textRight(snapshot_.model + "  " + snapshot_.address, right,
                             layout.top + layout.detail_y, layout.small_size,
                             col::dim);
        }
        return;
    }

    canvas.text("wristctl", layout.left + layout.margin,
                layout.top + layout.title_y, layout.title_size, col::text);
    canvas.text(truncateToWidth(canvas, status_line(snapshot_),
                                layout.content_w, layout.small_size),
                layout.left + layout.margin, layout.top + layout.status_y,
                layout.small_size, statusColor(snapshot_));
    const std::string detail =
        !snapshot_.error.empty()
            ? snapshot_.error
            : (snapshot_.connected
                   ? snapshot_.model + "  " + snapshot_.address
                   : std::string());
    if (!detail.empty()) {
        canvas.text(truncateToWidth(canvas, detail, layout.content_w,
                                    layout.small_size),
                    layout.left + layout.margin, layout.top + layout.detail_y,
                    layout.small_size,
                    snapshot_.error.empty() ? col::dim : col::red);
    }
}

void WatchView::drawTabs(Canvas &canvas, const Layout &layout)
{
    for (int i = 0; i < kPageCount; i++) {
        const int row = i / layout.tab_cols;
        const int col = i % layout.tab_cols;
        const float x = layout.left + layout.margin +
                        col * (layout.tab_w + layout.tab_gap);
        const float y = layout.top + layout.tabs_y +
                        row * (layout.tab_h + layout.tab_gap);
        addButton(canvas, x, y, layout.tab_w, layout.tab_h, kPageNames[i],
                  Action::Page, i, page_ == i,
                  touch_ ? layout.tab_h * 0.18f : 7.0f);
    }
}

void WatchView::drawActions(Canvas &canvas, const Layout &layout)
{
    const float y = layout.top + layout.actions_y;
    const float radius = touch_ ? layout.action_h * 0.2f : 7.0f;

    if (!snapshot_.connected) {
        if (!snapshot_.supports_scan || snapshot_.demo) {
            const float w = touch_ ? std::min(200 * display_density_,
                                              layout.content_w)
                                   : 110.0f;
            addButton(canvas, layout.left + layout.content_x, y, w,
                      layout.action_h,
                      snapshot_.busy ? "Scanning..." : "Connect",
                      Action::Connect, 0, false, radius);
            return;
        }

        const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
        const float gap = 8 * d;
        const float w = (layout.content_w - gap) / 2;
        float x = layout.left + layout.content_x;
        const bool scanning = snapshot_.scanning;
        addButton(canvas, x, y, w, layout.action_h,
                  scanning ? "Stop scan" : "Scan",
                  scanning ? Action::StopScan : Action::Scan, 0, false, radius);
        x += w + gap;
        if (!selected_address_.empty()) {
            addButton(canvas, x, y, w, layout.action_h,
                      selected_is_bonded() ? "Connect" : "Pair & connect",
                      Action::ConnectSelected, 0, false, radius);
        } else {
            canvas.button(x, y, w, layout.action_h, "Pair & connect",
                          col::btnWait, col::dim, radius);
        }
        return;
    }

    if (!touch_) {
        float x = layout.left + 20;
        addButton(canvas, x, y, 110, layout.action_h, "Disconnect",
                  Action::Disconnect, 0, false, radius);
        x += 118;
        addButton(canvas, x, y, 90, layout.action_h, "Refresh",
                  Action::Refresh, 0, false, radius);
        x += 98;
        addButton(canvas, x, y, 100, layout.action_h, "Sync time",
                  Action::SyncTime, 0, false, radius);
        return;
    }

    const float gap = 8 * display_density_;
    const float w = (layout.content_w - 2 * gap) / 3;
    float x = layout.left + layout.content_x;
    auto action_button = [&](const char *label, Action action) {
        canvas.button(x, y, w, layout.action_h, "", col::btnIdle, col::text,
                      radius);
        const float size = layout.text_size;
        const float tw = canvas.textWidth(label, size);
        canvas.text(label, x + (w - tw) * 0.5f,
                    y + (layout.action_h - size) * 0.5f, size, col::text);
        hits_.push_back({{x, y, w, layout.action_h}, action, 0, {}});
        x += w + gap;
    };
    action_button("Disconnect", Action::Disconnect);
    action_button("Refresh", Action::Refresh);
    action_button("Sync time", Action::SyncTime);
}

float WatchView::drawWatchPage(Canvas &canvas, const Layout &layout)
{
    if (!snapshot_.connected && snapshot_.supports_scan && !snapshot_.demo) {
        return drawPairPage(canvas, layout);
    }

    const float scroll = page_scroll(page_);
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    float cursor = top;

    auto row = [&](const char *label, const std::string &value,
                   Color color = col::text) {
        canvas.text(label, x, cursor - scroll, layout.small_size, col::dim);
        const std::string shown = truncateToWidth(
            canvas, value, layout.content_w - layout.label_w, layout.text_size);
        canvas.text(shown, x + layout.label_w, cursor - scroll,
                    layout.text_size, color);
        cursor += layout.row_line;
    };

    row("name", snapshot_.name.empty() ? "-" : snapshot_.name);
    row("model", snapshot_.model.empty() ? "-" : snapshot_.model);
    row("address", snapshot_.address.empty() ? "-" : snapshot_.address);
    row("pairing", snapshot_.pairing.empty() ? "-" : snapshot_.pairing);
    row("module frame", frame_text(snapshot_.module_frame));
    row("version frame", frame_text(snapshot_.version_frame));
    if (snapshot_.has_condition) {
        row("battery", std::to_string(snapshot_.battery));
        row("temperature", std::to_string(snapshot_.temperature) + " C");
        row("MTU", std::to_string(snapshot_.mtu));
    } else {
        row("condition", "not read yet");
    }
    if (snapshot_.has_settings) {
        char flags[32];
        std::snprintf(flags, sizeof flags, "0x%02x", snapshot_.settings_flags);
        row("settings", std::string(flags) +
                            (snapshot_.time_24h ? "  24h" : "  12h"));
    } else {
        row("settings", "not read yet");
    }

    if (snapshot_.demo) {
        cursor += 8;
        const float bottom = drawWrapped(
            canvas,
            "Demo data from the recorded session; press Connect for a real "
            "watch.",
            x, cursor - scroll, layout.content_w, layout.small_size, col::dim,
            layout.small_size * 1.25f);
        cursor = bottom + scroll;
    }

    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    cursor += layout.row_line * 0.4f;
    const float auto_w = std::min(layout.content_w, 250 * d);
    addButton(canvas, x, cursor - scroll, auto_w, layout.action_h,
              snapshot_.auto_connect ? "Auto-connect: On"
                                     : "Auto-connect: Off",
              Action::ToggleAutoConnect, 0, false,
              touch_ ? layout.action_h * 0.2f : 7.0f);
    cursor += layout.action_h + 4 * d;
    return cursor - top;
}

float WatchView::drawPairPage(Canvas &canvas, const Layout &layout)
{
    const float scroll = page_scroll(page_);
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float w = layout.content_w;
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = 8 * d;
    const float btn_h = touch_ ? 44 * d : 30.0f;
    const float row_h = touch_ ? 64 * d : 46.0f;
    float cursor = top;

    auto line = [&](const std::string &text, Color color, float size) {
        const float bottom = drawWrapped(canvas, text, x, cursor - scroll, w,
                                         size, color, size * 1.4f);
        cursor = bottom + scroll + 4 * d;
    };
    auto banner_button = [&](float bx, float by, float bw,
                             const char *label, Action action) {
        addButton(canvas, bx, by, bw, btn_h, label, action, 0, false,
                  btn_h * 0.2f);
    };

    const bool blocked = snapshot_.permission_missing ||
                         snapshot_.bluetooth_off || snapshot_.location_off;
    if (snapshot_.permission_missing) {
        line("Nearby devices permission is required to find and pair your "
             "watch.",
             col::yellow, layout.small_size);
        const float half = (w - gap) / 2;
        banner_button(x, cursor - scroll, half, "Grant", Action::Grant);
        banner_button(x + half + gap, cursor - scroll, half, "Open settings",
                      Action::OpenSettings);
        cursor += btn_h + gap;
    }
    if (snapshot_.bluetooth_off) {
        line("Bluetooth is off.", col::yellow, layout.small_size);
        banner_button(x, cursor - scroll, std::min(w, 200 * d), "Turn on",
                      Action::TurnOnBluetooth);
        cursor += btn_h + gap;
    }
    if (snapshot_.location_off) {
        line("Location services are off; Android 11 and older need them for BLE "
             "scanning.",
             col::yellow, layout.small_size);
        banner_button(x, cursor - scroll, std::min(w, 230 * d),
                      "Location settings", Action::OpenLocation);
        cursor += btn_h + gap;
    }
    if (!snapshot_.error.empty() && !blocked) {
        line(snapshot_.error, col::red, layout.small_size);
    }

    const bool looking =
        snapshot_.scanning ||
        snapshot_.status.find("scanning for the watch") != std::string::npos ||
        snapshot_.status.find("watching for the watch") != std::string::npos ||
        snapshot_.status.find("connecting") != std::string::npos;
    if (looking) {
        line(snapshot_.scanning
                 ? "Scanning... press a button on the watch to wake it."
                 : "Looking for your watch... press a button on the watch.",
             col::dim, layout.small_size);
    } else if (!blocked && snapshot_.devices.empty()) {
        line(snapshot_.status == "not connected" ||
                     snapshot_.status == "disconnected"
                 ? "Tap Scan to find and pair your watch."
                 : "No watches found. Press a button on the watch, then Scan "
                   "again.",
             col::dim, layout.small_size);
    } else if (!blocked) {
        line("Select a watch, then Pair & connect.", col::dim,
             layout.small_size);
    }

    std::vector<const WatchDevice *> visible;
    for (const WatchDevice &device : snapshot_.devices) {
        if (show_all_ || looks_like_watch_name(device.name)) {
            visible.push_back(&device);
        }
    }
    std::stable_sort(visible.begin(), visible.end(),
                     [](const WatchDevice *a, const WatchDevice *b) {
                         if (a->bonded != b->bonded) {
                             return a->bonded;
                         }
                         return a->name < b->name;
                     });

    if (!visible.empty()) {
        canvas.text("Nearby watches", x, cursor - scroll,
                    layout.page_title_size, col::text);
        cursor += layout.page_title_size * 1.5f;
    }

    for (const WatchDevice *device : visible) {
        const Rect row{x, cursor - scroll, w, row_h};
        const bool selected = device->address == selected_address_;
        const Color selected_bg{0.18f, 0.32f, 0.60f, 1.0f};
        const Color detail_color{0.82f, 0.88f, 1.0f, 1.0f};
        canvas.rect(row.x, row.y, row.w, row.h,
                    selected ? selected_bg : col::panel2, 8 * d);
        canvas.text(truncateToWidth(canvas, device->name, w - 100 * d,
                                    layout.text_size),
                    x + 12 * d, row.y + 8 * d, layout.text_size, col::text);
        std::string detail = device->address;
        if (device->rssi != 0) {
            detail += "   " + std::to_string(device->rssi) + " dBm";
        }
        canvas.text(truncateToWidth(canvas, detail, w - 100 * d,
                                    layout.small_size),
                    x + 12 * d, row.y + 32 * d, layout.small_size,
                    selected ? detail_color : col::dim);
        const char *badge = device->bonded ? "Paired" : "New";
        const float badge_w = canvas.textWidth(badge, layout.small_size);
        canvas.text(badge, row.x + w - 12 * d - badge_w, row.y + 10 * d,
                    layout.small_size,
                    selected ? col::text
                             : (device->bonded ? col::green : col::dim));
        addHit(row, Action::SelectDevice, device->address);
        cursor += row_h + gap;
    }

    if (!snapshot_.devices.empty()) {
        const float bw = std::min(w, 220 * d);
        addButton(canvas, x, cursor - scroll, bw, btn_h,
                  show_all_ ? "Show Casio only" : "Show all devices",
                  Action::ToggleAll, 0, false, btn_h * 0.2f);
        cursor += btn_h + gap;
    }

    if (!snapshot_.devices.empty() && visible.empty()) {
        line("No Casio watches in the list; tap Show all devices.", col::dim,
             layout.small_size);
    }

    const float auto_w = std::min(w, 250 * d);
    addButton(canvas, x, cursor - scroll, auto_w, btn_h,
              snapshot_.auto_connect ? "Auto-connect: On"
                                     : "Auto-connect: Off",
              Action::ToggleAutoConnect, 0, false, btn_h * 0.2f);
    cursor += btn_h + gap;
    line("When on, the app reconnects to a paired watch on launch.", col::dim,
         layout.small_size);

    return cursor - top;
}

float WatchView::drawClockPage(Canvas &canvas, const Layout &layout)
{
    auto display_now = std::chrono::system_clock::now();
    const bool corrected = settings_.use_ntp && snapshot_.has_time_sample;
    if (corrected) {
        display_now = snapshot_.time_corrected_at +
                      std::chrono::duration_cast<
                          std::chrono::system_clock::duration>(
                          std::chrono::steady_clock::now() -
                          snapshot_.time_sampled_at);
    }
    std::tm calendar{};
    if (settings_.use_ntp) {
        calendar = utc_tm(display_now);
    } else {
        const std::time_t seconds =
            std::chrono::system_clock::to_time_t(display_now);
        localtime_r(&seconds, &calendar);
    }

    const bool show_24h = !snapshot_.has_settings ||
                          edit_settings_.time_format_24h != 0;
    char clock[32];
    char date[64];
    std::strftime(clock, sizeof clock,
                  settings_.app_time_24h ? "%H:%M:%S" : "%I:%M:%S %p",
                  &calendar);
    std::strftime(date, sizeof date, "%A %d %B %Y", &calendar);

    std::string zone_label;
    if (corrected) {
        zone_label = "SNTP UTC: " + snapshot_.time_server;
    } else if (settings_.use_ntp) {
        zone_label = "UTC preview uses the device clock until SNTP succeeds";
    } else {
        const char *zone = std::getenv("TZ");
        zone_label = (zone != nullptr && *zone != '\0')
                         ? std::string("watch timezone: ") + zone
                         : std::string(touch_
                                           ? "device local time"
                                           : "host local time (no --timezone set)");
    }

    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float scroll = page_scroll(page_);
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = 8 * d;
    const float btn_h = touch_ ? 40 * d : 30.0f;
    const float half = (layout.content_w - gap) / 2;
    float cursor = top;

    canvas.text(clock, x, cursor - scroll, layout.clock_size, col::text);
    cursor += layout.clock_size * 1.1f;
    canvas.text(date, x, cursor - scroll, layout.date_size, col::dim);
    cursor += layout.date_size * 1.6f;
    cursor = drawWrapped(canvas, zone_label, x, cursor - scroll,
                         layout.content_w, layout.small_size, col::dim,
                         layout.small_size * 1.4f) +
             scroll + gap;

    addButton(canvas, x, cursor - scroll, half, btn_h,
              settings_.use_ntp ? "Source: SNTP UTC" : "Source: device local",
              Action::ToggleTimeSource, 0, settings_.use_ntp, btn_h * 0.18f);
    addButton(canvas, x + half + gap, cursor - scroll, half, btn_h,
              settings_.sync_on_connect ? "Sync on connect: On"
                                        : "Sync on connect: Off",
              Action::ToggleSyncOnConnect, 0, settings_.sync_on_connect,
              btn_h * 0.18f);
    cursor += btn_h + gap;

    addButton(canvas, x, cursor - scroll, half, btn_h, "Edit SNTP servers",
              Action::EditField, 0, false, btn_h * 0.18f, EDIT_NTP_SERVERS);
    addButton(canvas, x + half + gap, cursor - scroll, half, btn_h, "Sync now",
              Action::SyncTime, 0, false, btn_h * 0.18f);
    cursor += btn_h + gap;

    if (settings_.use_ntp) {
        cursor = drawWrapped(canvas, "Servers, in order: " + ntp_server_text_, x,
                             cursor - scroll, layout.content_w,
                             layout.small_size, col::dim,
                             layout.small_size * 1.4f) +
                 scroll + gap;
    }

    if (snapshot_.has_settings) {
        addButton(canvas, x, cursor - scroll, half, btn_h,
                  show_24h ? "Watch display: 24H" : "Watch display: 12H",
                  Action::SettingToggle, 0, show_24h, btn_h * 0.18f, 0);
        addButton(canvas, x + half + gap, cursor - scroll, half, btn_h,
                  "Apply display format", Action::SettingsApply, 0,
                  settings_dirty_, btn_h * 0.18f);
        cursor += btn_h + gap;
    }

    addButton(canvas, x, cursor - scroll, layout.content_w, btn_h,
              settings_.app_time_24h ? "App display: 24H" : "App display: 12H",
              Action::ToggleAppTimeFormat, 0, settings_.app_time_24h,
              btn_h * 0.18f);
    cursor += btn_h + gap;

    if (snapshot_.has_clock) {
        cursor = drawSectionTitle(canvas, layout, "Cities and DST", x,
                                  cursor - scroll) +
                 scroll;
        static const char *const units[] = {"Home", "World", "City 3",
                                            "City 4", "City 5"};
        const float unit_w = (layout.content_w - 4 * gap) / 5;
        for (int unit = 0; unit < 5; unit++) {
            addButton(canvas, x + unit * (unit_w + gap), cursor - scroll, unit_w,
                      btn_h, units[unit], Action::ClockUnit, 0,
                      clock_unit_ == unit, btn_h * 0.18f, unit);
        }
        cursor += btn_h + gap;

        const wc_dst_setting &city = edit_dst_settings_[clock_unit_];
        char city_id[16];
        std::snprintf(city_id, sizeof city_id, "0x%04x", city.city);
        cursor = drawKeyValue(canvas, layout, "City ID (preserved)", city_id, x,
                              cursor - scroll) +
                 scroll;
        canvas.text("City label", x, cursor - scroll, layout.small_size,
                    col::dim);
        canvas.text(truncateToWidth(canvas, edit_city_name_,
                                    layout.content_w - layout.label_w - 90 * d,
                                    layout.text_size),
                    x + layout.label_w, cursor - scroll, layout.text_size,
                    col::text);
        addButton(canvas, x + layout.content_w - 80 * d, cursor - scroll - 4 * d,
                  80 * d, btn_h, "Edit", Action::EditField, 0, false,
                  btn_h * 0.18f, EDIT_CITY_NAME);
        cursor += btn_h + gap;

        cursor = drawKeyValue(canvas, layout, "UTC offset",
                              offset_text(city.timezone, true), x,
                              cursor - scroll) +
                 scroll;
        addButton(canvas, x, cursor - scroll, half, btn_h, "-15 minutes",
                  Action::CityTimezone, 0, false, btn_h * 0.18f, -1);
        addButton(canvas, x + half + gap, cursor - scroll, half, btn_h,
                  "+15 minutes", Action::CityTimezone, 0, false, btn_h * 0.18f,
                  1);
        cursor += btn_h + gap;

        cursor = drawKeyValue(canvas, layout, "DST difference",
                              offset_text(city.dst_diff, false), x,
                              cursor - scroll) +
                 scroll;
        addButton(canvas, x, cursor - scroll, half, btn_h, "-15 minutes",
                  Action::CityDstDiff, 0, false, btn_h * 0.18f, -1);
        addButton(canvas, x + half + gap, cursor - scroll, half, btn_h,
                  "+15 minutes", Action::CityDstDiff, 0, false, btn_h * 0.18f,
                  1);
        cursor += btn_h + gap;

        if (clock_unit_ < 2) {
            const bool dst_on = clock_unit_ == 0
                                    ? edit_dst_state_.dst_on_home != 0
                                    : edit_dst_state_.dst_on_world != 0;
            const bool dst_auto = clock_unit_ == 0
                                      ? edit_dst_state_.dst_auto_home != 0
                                      : edit_dst_state_.dst_auto_world != 0;
            addButton(canvas, x, cursor - scroll, half, btn_h,
                      dst_on ? "DST: On" : "DST: Off", Action::CityDstToggle, 0,
                      dst_on, btn_h * 0.18f, 0);
            addButton(canvas, x + half + gap, cursor - scroll, half, btn_h,
                      dst_auto ? "Auto DST: On" : "Auto DST: Off",
                      Action::CityDstToggle, 0, dst_auto, btn_h * 0.18f, 1);
            cursor += btn_h + gap;
        }

        addButton(canvas, x, cursor - scroll, half, layout.action_h,
                  clock_confirm_ ? "Tap again to write city" : "Apply city",
                  Action::CityApply, 0, clock_dirty_, layout.action_h * 0.2f);
        addButton(canvas, x + half + gap, cursor - scroll, half, layout.action_h,
                  "Revert cities", Action::CityRevert, 0, false,
                  layout.action_h * 0.2f);
        cursor += layout.action_h + gap;
        cursor = drawWrapped(
                     canvas,
                     "City writes preserve the numeric city ID and unread "
                     "protocol fields. UTC and DST offsets use 15-minute units. "
                     "Apply is confirmed twice and reads all five slots back.",
                     x, cursor - scroll, layout.content_w, layout.small_size,
                     col::dim, layout.small_size * 1.4f) +
                 scroll + gap;
    }

    if (snapshot_.has_time_sample) {
        char diagnostics[160];
        std::snprintf(diagnostics, sizeof diagnostics,
                      "Last SNTP sample: %s  offset %.3f ms  RTT %.3f ms",
                      snapshot_.time_server.c_str(), snapshot_.time_offset_ms,
                      snapshot_.time_round_trip_ms);
        cursor = drawWrapped(canvas, diagnostics, x, cursor - scroll,
                             layout.content_w, layout.small_size, col::dim,
                             layout.small_size * 1.4f) +
                 scroll + gap;
    }
    if (settings_.use_ntp && !snapshot_.time_error.empty()) {
        cursor = drawWrapped(canvas, snapshot_.time_error, x, cursor - scroll,
                             layout.content_w, layout.small_size, col::red,
                             layout.small_size * 1.4f) +
                 scroll + gap;
    }

    cursor = drawWrapped(
                 canvas,
                 settings_.use_ntp
                     ? "Sync sets Home and World to UTC, preserving their city "
                       "IDs, then writes fresh SNTP UTC time. This changes the "
                       "watch, not the phone or host system clock. 0x09 itself "
                       "has no readback."
                     : "Sync writes device local time to the watch. The watch "
                       "stores no readback for 0x09; verify its display.",
                 x, cursor - scroll, layout.content_w, layout.small_size,
                 col::dim, layout.small_size * 1.4f) +
             scroll;
    return cursor - top;
}

float WatchView::drawAlarmsPage(Canvas &canvas, const Layout &layout)
{
    const float scroll = page_scroll(page_);
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = 6 * d;
    const float btn_h = touch_ ? 40 * d : 30.0f;
    float cursor = top;

    canvas.text("Alarms", x, cursor - scroll, layout.page_title_size,
                col::text);
    cursor += layout.page_title_size * 1.6f;

    if (!snapshot_.has_alarms) {
        cursor = drawWrapped(canvas, "not read yet; connect and press Refresh",
                             x, cursor - scroll, layout.content_w,
                             layout.text_size, col::dim, layout.row_line) +
                 scroll;
        return cursor - top;
    }

    auto time_text = [](uint8_t hour, uint8_t minute) {
        char text[16];
        std::snprintf(text, sizeof text, "%02u:%02u", hour, minute);
        return std::string(text);
    };

    for (int index = 0; index < 5; index++) {
        const bool first = index == 0;
        const uint8_t enabled =
            first ? edit_first_.enabled : edit_rest_.alarms[index - 1].enabled;
        const uint8_t one_time =
            first ? edit_first_.one_time : edit_rest_.alarms[index - 1].one_time;
        const uint8_t chime =
            first ? edit_first_.hourly_chime
                  : edit_rest_.alarms[index - 1].hourly_chime;
        const uint8_t hour =
            first ? edit_first_.hour : edit_rest_.alarms[index - 1].hour;
        const uint8_t minute =
            first ? edit_first_.minute : edit_rest_.alarms[index - 1].minute;

        char label[32];
        std::snprintf(label, sizeof label, "Alarm %d", index + 1);
        canvas.text(label, x, cursor - scroll, layout.text_size, col::text);
        const std::string time = time_text(hour, minute);
        canvas.text(time, x + layout.label_w, cursor - scroll,
                    layout.text_size, enabled ? col::green : col::dim);
        const std::string state =
            std::string(enabled ? "on" : "off") + "  " +
            (enabled ? (one_time ? "once" : "daily") : "   ") +
            (first ? (chime ? "  chime" : "  silent") : "");
        canvas.text(state, x + layout.label_w + 90 * d, cursor - scroll,
                    layout.small_size, col::dim);
        cursor += layout.row_line;

        const int count = first ? 7 : 6;
        const float bw =
            (layout.content_w - gap * static_cast<float>(count - 1)) / count;
        float bx = x;
        auto button = [&](const std::string &text, Action action, int arg,
                          int arg2, bool active) {
            addButton(canvas, bx, cursor - scroll, bw, btn_h, text, action, 0,
                      active, btn_h * 0.18f, arg, arg2);
            bx += bw + gap;
        };
        button("-1h", Action::AlarmHour, index, -1, false);
        button("+1h", Action::AlarmHour, index, 1, false);
        button("-5m", Action::AlarmMinute, index, -5, false);
        button("+5m", Action::AlarmMinute, index, 5, false);
        button(enabled ? "On" : "Off", Action::AlarmToggle, index, 0, enabled);
        button(one_time ? "Once" : "Daily", Action::AlarmMode, index, 0,
               one_time);
        if (first) {
            button(chime ? "Chime" : "Silent", Action::AlarmChime, 0, 0, chime);
        }
        cursor += btn_h + gap;
    }

    cursor += 4 * d;
    const float half = (layout.content_w - gap) / 2;
    addButton(canvas, x, cursor - scroll, half, layout.action_h,
              alarms_confirm_ ? "Tap again to write" : "Apply alarms",
              Action::AlarmApply, 0, alarms_dirty_, layout.action_h * 0.2f);
    addButton(canvas, x + half + gap, cursor - scroll, half, layout.action_h,
              "Revert", Action::AlarmRevert, 0, false,
              layout.action_h * 0.2f);
    cursor += layout.action_h + gap;
    cursor = drawWrapped(
                 canvas,
                 "Alarms are written as two frames, 0x15 and 0x16. Apply keeps "
                 "the watch's own constant byte, so a reset alarm stays reset "
                 "until you change it.",
                 x, cursor - scroll, layout.content_w, layout.small_size,
                 col::dim, layout.small_size * 1.4f) +
             scroll;
    return cursor - top;
}

float WatchView::drawTimerPage(Canvas &canvas, const Layout &layout)
{
    const float scroll = page_scroll(page_);
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = 6 * d;
    const float btn_h = touch_ ? 40 * d : 30.0f;
    float cursor = top;

    canvas.text("Program timer", x, cursor - scroll, layout.page_title_size,
                col::text);
    cursor += layout.page_title_size * 1.6f;

    if (!snapshot_.has_timer) {
        cursor = drawWrapped(canvas, "not read yet; connect and press Refresh",
                             x, cursor - scroll, layout.content_w,
                             layout.text_size, col::dim, layout.row_line) +
                 scroll;
        return cursor - top;
    }

    canvas.text("repeat", x, cursor - scroll, layout.text_size, col::dim);
    char repeat[32];
    std::snprintf(repeat, sizeof repeat, "%u", edit_timer_.repeat);
    canvas.text(repeat, x + layout.label_w, cursor - scroll,
                layout.text_size, col::text);
    const float rw = 110 * d;
    addButton(canvas, x + layout.content_w - rw * 2 - gap, cursor - scroll, rw,
              btn_h, "-1", Action::TimerRepeat, 0, false, btn_h * 0.18f, -1);
    addButton(canvas, x + layout.content_w - rw, cursor - scroll, rw, btn_h,
              "+1", Action::TimerRepeat, 0, false, btn_h * 0.18f, 1);
    cursor += btn_h + gap;

    canvas.text("one round", x, cursor - scroll, layout.text_size, col::dim);
    canvas.text(duration_text(timer_round_seconds(edit_timer_)),
                x + layout.label_w, cursor - scroll, layout.text_size, col::text);
    cursor += layout.row_line + gap;

    static const char *const beep_names[] = {"Walk1", "Walk2", "Rush", "Rest"};
    for (int index = 0; index < 6; index++) {
        const wc_program_timer_workouts_item &item =
            edit_timer_.workouts[index];
        char label[32];
        std::snprintf(label, sizeof label, "%d", index + 1);
        canvas.text(label, x, cursor - scroll, layout.text_size, col::dim);
        char time[32];
        std::snprintf(time, sizeof time, "%02u:%02u", item.minutes,
                      item.seconds);
        canvas.text(time, x + 26 * d, cursor - scroll, layout.text_size,
                    (item.minutes == 0 && item.seconds == 0) ? col::dim
                                                             : col::text);
        canvas.text(beep_names[item.beep & 3], x + 110 * d, cursor - scroll,
                    layout.small_size, col::dim);
        cursor += layout.row_line;

        const int count = 5;
        const float bw = (layout.content_w - gap * (count - 1)) / count;
        float bx = x;
        auto button = [&](const std::string &text, Action action, int arg,
                          int arg2) {
            addButton(canvas, bx, cursor - scroll, bw, btn_h, text, action, 0,
                      false, btn_h * 0.18f, arg, arg2);
            bx += bw + gap;
        };
        button("-1m", Action::TimerMinutes, index, -1);
        button("+1m", Action::TimerMinutes, index, 1);
        button("-5s", Action::TimerSeconds, index, -5);
        button("+5s", Action::TimerSeconds, index, 5);
        button("Sound", Action::TimerBeep, index, 1);
        cursor += btn_h + gap;
    }

    cursor += 4 * d;
    const float third = (layout.content_w - 2 * gap) / 3;
    addButton(canvas, x, cursor - scroll, third, layout.action_h,
              "Initialize", Action::TimerInitialize, 0, false,
              layout.action_h * 0.2f);
    addButton(canvas, x + third + gap, cursor - scroll, third, layout.action_h,
              timer_confirm_ ? "Tap again to replace" : "Apply timer",
              Action::TimerApply, 0, timer_dirty_, layout.action_h * 0.2f);
    addButton(canvas, x + 2 * (third + gap), cursor - scroll, third,
              layout.action_h, "Revert", Action::TimerRevert, 0, false,
              layout.action_h * 0.2f);
    cursor += layout.action_h + gap;
    cursor = drawWrapped(
                 canvas,
                 "Apply replaces the whole program timer, including the repeat "
                 "count and every workout. The watch normalises the sound "
                 "byte; read it back before relying on a value other than "
                 "Walk1.",
                 x, cursor - scroll, layout.content_w, layout.small_size,
                 col::dim, layout.small_size * 1.4f) +
             scroll;
    return cursor - top;
}

float WatchView::drawNotifyPage(Canvas &canvas, const Layout &layout)
{
    const float scroll = page_scroll(page_);
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = 8 * d;
    const float half = (layout.content_w - gap) / 2;
    float cursor = drawSectionTitle(canvas, layout, "Phone & notifications", x,
                                    top - scroll) + scroll;

    if (snapshot_.supports_scan) {
        cursor = drawKeyValue(canvas, layout, "Notification access",
                              snapshot_.notification_access
                                  ? "granted"
                                  : "not granted (connect to refresh)",
                              x, cursor - scroll,
                              snapshot_.notification_access ? col::green
                                                            : col::yellow) +
                 scroll;
        addButton(canvas, x, cursor - scroll, half, layout.action_h,
                  settings_.notify_forward ? "Forwarding: On" : "Forwarding: Off",
                  Action::ToggleNotifyForward, 0, settings_.notify_forward,
                  layout.action_h * 0.2f);
        addButton(canvas, x + half + gap, cursor - scroll, half, layout.action_h,
                  "Open notification access", Action::OpenNotificationAccess, 0,
                  false, layout.action_h * 0.2f);
        cursor += layout.action_h + gap;
        addButton(canvas, x, cursor - scroll, half, layout.action_h,
                  "Edit allowed apps", Action::EditField, 0, false,
                  layout.action_h * 0.2f, EDIT_NOTIFY_PACKAGES);
        char quiet[48] = "Quiet hours: Off";
        if (settings_.notify_quiet_start_min >= 0 &&
            settings_.notify_quiet_end_min >= 0) {
            std::snprintf(quiet, sizeof quiet, "Quiet: %02d:%02d-%02d:%02d",
                          settings_.notify_quiet_start_min / 60,
                          settings_.notify_quiet_start_min % 60,
                          settings_.notify_quiet_end_min / 60,
                          settings_.notify_quiet_end_min % 60);
        }
        addButton(canvas, x + half + gap, cursor - scroll, half, layout.action_h,
                  quiet, Action::NotifyQuietHours, 0, false,
                  layout.action_h * 0.2f);
        cursor += layout.action_h + gap;
        cursor = drawWrapped(canvas,
                             "Allowed packages (blank = all): " +
                                 (notify_packages_text_.empty()
                                      ? std::string("all")
                                      : notify_packages_text_),
                             x, cursor - scroll, layout.content_w,
                             layout.small_size, col::dim,
                             layout.small_size * 1.4f) +
                 scroll + gap;
        cursor = drawKeyValue(canvas, layout, "Local delivery",
                              std::to_string(snapshot_.notifications_forwarded) +
                                  " forwarded, " +
                                  std::to_string(snapshot_.notifications_filtered) +
                                  " filtered",
                              x, cursor - scroll) +
                 scroll + gap;
    } else {
        cursor = drawNote(canvas, layout,
                          "Automatic Android notification access is unavailable "
                          "on this desktop build. Manual test notifications "
                          "still work.",
                          x, cursor - scroll) +
                 scroll + gap;
    }

    cursor = drawSectionTitle(canvas, layout, "Phone finder", x,
                              cursor - scroll) + scroll;
    cursor = drawNote(canvas, layout,
                      "A high alert from the watch plays this phone's local "
                      "alarm sound for at most 60 seconds. Alert zero or "
                      "disconnecting stops it.",
                      x, cursor - scroll) +
             scroll;
    addButton(canvas, x, cursor - scroll, layout.content_w, layout.action_h,
              "Stop phone finder sound", Action::StopPhoneFinder, 0, false,
              layout.action_h * 0.2f);
    cursor += layout.action_h + gap;

    cursor = drawSectionTitle(canvas, layout, "Manual test", x,
                              cursor - scroll) + scroll;
    static const char *const types[] = {"Other", "Call", "Mail", "SNS"};
    auto field = [&](const char *label, const std::string &value,
                     int edit_field) {
        canvas.text(label, x, cursor - scroll, layout.small_size, col::dim);
        canvas.text(truncateToWidth(canvas, value, layout.content_w - 90 * d,
                                    layout.text_size),
                    x + 64 * d, cursor - scroll, layout.text_size, col::text);
        const float bw = 74 * d;
        addButton(canvas, x + layout.content_w - bw, cursor - scroll - 4 * d,
                  bw, touch_ ? 36 * d : 28, "Edit", Action::EditField, 0, false,
                  (touch_ ? 36 * d : 28) * 0.18f, edit_field);
        cursor += layout.row_line;
    };

    field("Type", types[notify_type_ & 3], -1);
    field("App", notify_app_, EDIT_NOTIFY_APP);
    field("Title", notify_title_, EDIT_NOTIFY_TITLE);
    field("Text", notify_text_, EDIT_NOTIFY_TEXT);
    cursor += 4 * d;
    addButton(canvas, x, cursor - scroll, half, layout.action_h,
              "Change type", Action::NotifyType, 0, false,
              layout.action_h * 0.2f);
    addButton(canvas, x + half + gap, cursor - scroll, half, layout.action_h,
              "Send now", Action::NotifySend, 0, false,
              layout.action_h * 0.2f);
    cursor += layout.action_h + gap;
    cursor = drawWrapped(
                 canvas,
                 "Forwarding is disabled by default and filtered on this "
                 "device; notification text is never uploaded. The 0x30 packet "
                 "has no readback, so confirm delivery on the watch. Its "
                 "notification setting must be enabled.",
                 x, cursor - scroll, layout.content_w, layout.small_size,
                 col::dim, layout.small_size * 1.4f) +
             scroll;
    return cursor - top;
}

float WatchView::drawSettingsPage(Canvas &canvas, const Layout &layout)
{
    const float scroll = page_scroll(page_);
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    const float d = touch_ ? std::max(1.0f, display_density_) : 1.0f;
    const float gap = 8 * d;
    float cursor = top;

    canvas.text("Settings", x, cursor - scroll, layout.page_title_size,
                col::text);
    cursor += layout.page_title_size * 1.6f;

    if (!snapshot_.has_settings) {
        cursor = drawWrapped(canvas, "not read yet; connect and press Refresh",
                             x, cursor - scroll, layout.content_w,
                             layout.text_size, col::dim, layout.row_line) +
                 scroll;
        return cursor - top;
    }

    const float btn_w = std::min(layout.content_w * 0.55f, 260 * d);
    const float row_h = touch_ ? 50 * d : 38.0f;
    auto toggle = [&](const char *label, bool on, int field) {
        canvas.text(label, x, cursor - scroll + (row_h - layout.text_size) / 2,
                    layout.text_size, col::text);
        addButton(canvas, x + layout.content_w - btn_w, cursor - scroll, btn_w,
                  touch_ ? 40 * d : 30, on ? "On" : "Off",
                  Action::SettingToggle, 0, on, (touch_ ? 40 * d : 30) * 0.18f,
                  field);
        cursor += row_h;
    };
    auto cycle = [&](const char *label, const std::string &value, int field) {
        canvas.text(label, x, cursor - scroll + (row_h - layout.text_size) / 2,
                    layout.text_size, col::text);
        addButton(canvas, x + layout.content_w - btn_w, cursor - scroll, btn_w,
                  touch_ ? 40 * d : 30, value, Action::SettingCycle, 0, false,
                  (touch_ ? 40 * d : 30) * 0.18f, field);
        cursor += row_h;
    };

    static const char *const languages[] = {"English",  "Español", "Français",
                                            "Deutsch", "Italiano", "Русский"};
    toggle("24-hour time", edit_settings_.time_format_24h != 0, 0);
    toggle("Button tone", edit_settings_.button_tone_off == 0, 1);
    toggle("Auto light", edit_settings_.auto_light_off == 0, 2);
    toggle("Power saving", edit_settings_.power_saving_off == 0, 3);
    cycle("Light duration",
          edit_settings_.light_duration != 0 ? "3 seconds" : "1 second", 4);
    cycle("Date format",
          edit_settings_.date_format_ddmm != 0 ? "DD/MM" : "MM/DD", 5);
    cycle("Language", languages[edit_settings_.language % 6], 6);

    canvas.text("Watch name", x,
                cursor - scroll + (row_h - layout.text_size) / 2,
                layout.text_size, col::text);
    canvas.text(truncateToWidth(canvas, edit_name_, btn_w - 90 * d,
                                layout.small_size),
                x + 140 * d, cursor - scroll + (row_h - layout.small_size) / 2,
                layout.small_size, col::dim);
    addButton(canvas, x + layout.content_w - btn_w, cursor - scroll, btn_w,
              touch_ ? 40 * d : 30, "Edit name", Action::EditField, 0, false,
              (touch_ ? 40 * d : 30) * 0.18f, EDIT_WATCH_NAME);
    cursor += row_h;

    cursor += 4 * d;
    const float half = (layout.content_w - gap) / 2;
    addButton(canvas, x, cursor - scroll, half, layout.action_h, "Apply settings",
              Action::SettingsApply, 0, settings_dirty_ || name_dirty_,
              layout.action_h * 0.2f);
    addButton(canvas, x + half + gap, cursor - scroll, half, layout.action_h,
              "Revert", Action::SettingsRevert, 0, false,
              layout.action_h * 0.2f);
    cursor += layout.action_h + gap;
    cursor = drawWrapped(
                 canvas,
                 "Display, sound and light flags are a read-modify-write of "
                 "0x13: unknown bits are preserved. The name write is 0x23; "
                 "the GBX-100 accepts 19 bytes.",
                 x, cursor - scroll, layout.content_w, layout.small_size,
                 col::dim, layout.small_size * 1.4f) +
             scroll;
    return cursor - top;
}

float WatchView::drawConsolePage(Canvas &canvas, const Layout &layout)
{
    const float scroll = page_scroll(page_);
    const float top = layout.top + layout.content_y;
    const float x = layout.left + layout.content_x;
    float cursor = top;

    canvas.text("Console", x, cursor - scroll, layout.page_title_size,
                col::text);
    cursor += layout.page_title_size * 1.6f;

    const auto &log = snapshot_.log;
    const size_t limit = touch_ ? 40 : 16;
    const size_t start = log.size() > limit ? log.size() - limit : 0;
    const float line = layout.small_size * 1.45f;
    for (size_t i = start; i < log.size(); i++) {
        const std::string shown =
            truncateToWidth(canvas, log[i], layout.content_w,
                            layout.small_size);
        canvas.text(shown, x, cursor - scroll, layout.small_size, col::dim);
        cursor += line;
    }
    return cursor - top;
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
               : "1-9,0 pages  C connect/auto  R refresh  T sync  Esc quit";
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
        switch (it->action) {
        case Action::Connect:
            worker_.connect();
            break;
        case Action::Scan:
            selected_address_.clear();
            page_ = 1;
            settings_.route = route_from_legacy_page(page_);
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
            page_ = it->page;
            settings_.route = route_from_legacy_page(page_);
            save_settings(settings_);
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
            if (!clock_dirty_ && it->arg >= 0 && it->arg < 5) {
                clock_unit_ = it->arg;
                edit_city_name_ = edit_city_names_[clock_unit_].name;
                clock_confirm_ = false;
            }
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
    clamp_scroll(page_, static_cast<float>(last_page_height_), clip_h);

    canvas.setClip(layout.left, clip_y, layout.content_w, clip_h);
    float page_height = 0;
    switch (page_) {
    case 0:
        page_height = drawHomePage(canvas, layout);
        break;
    case 1:
        page_height = drawWatchPage(canvas, layout);
        break;
    case 2:
        page_height = drawClockPage(canvas, layout);
        break;
    case 3:
        page_height = drawAlarmsPage(canvas, layout);
        break;
    case 4:
        page_height = drawTimerPage(canvas, layout);
        break;
    case 5:
        page_height = drawTrainingPage(canvas, layout);
        break;
    case 6:
        page_height = drawActivityPage(canvas, layout);
        break;
    case 7:
        page_height = drawNotifyPage(canvas, layout);
        break;
    case 8:
        page_height = drawSettingsPage(canvas, layout);
        break;
    case 9:
        page_height = drawConsolePage(canvas, layout);
        break;
    default:
        break;
    }
    canvas.clearClip();
    last_page_height_ = static_cast<int>(page_height);
    last_viewport_h_ = static_cast<int>(clip_h);

    // Chrome is drawn AFTER the content so the clip's tile-granular bleed can
    // never paint over a header or footer.
    drawHeader(canvas, layout);
    drawTabs(canvas, layout);
    drawActions(canvas, layout);
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
    for (int key = 0; key < kPageCount; key++) {
        const int code = key == 9 ? 0x30 : 0x31 + key;
        if (input().keyWentDown(code)) {
            page_ = key;
            settings_.route = route_from_legacy_page(page_);
            save_settings(settings_);
            dirty_ = true;
        }
    }
    if (input().keyWentDown(0x43)) { // C
        settings_.auto_connect = true;
        save_settings(settings_);
        worker_.set_auto_connect(true);
        worker_.connect();
        dirty_ = true;
    }
    if (input().keyWentDown(0x52)) { // R
        worker_.refresh();
        dirty_ = true;
    }
    if (input().keyWentDown(0x54)) { // T
        worker_.sync_time();
        dirty_ = true;
    }
    if (input().keyWentDown(key::Escape)) {
        running_ = false;
    }
}

} // namespace wristctl::gui
