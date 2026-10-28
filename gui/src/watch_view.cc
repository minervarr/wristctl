#include "watch_view.hh"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "canvas.hh"
#include "host.hh"
#include "keys.hh"
#include "raster_font.hh"
#include "renderer.hh"
#include "settings.hh"

namespace wristctl::gui {
namespace {

constexpr int kPollTimer = 1;
constexpr float kText = 18.0f;
constexpr float kSmall = 14.0f;
constexpr float kTitle = 28.0f;

const char *const kPageNames[] = {"Watch", "Clock", "Alarms", "Timer", "Notify", "Console"};

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

} // namespace

WatchView::WatchView() = default;
WatchView::~WatchView() = default;

bool WatchView::create()
{
    host_ = make_host();
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
    snapshot_ = worker_.snapshot();
    page_ = load_settings().page;
    seen_version_ = snapshot_.version;
    last_second_ = static_cast<long long>(std::time(nullptr));
    host_->startTimer(kPollTimer, 100);
    host_->showWindow();
    dirty_ = true;
    return true;
}

void WatchView::connect_now()
{
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
        if (!dirty_) {
            continue;
        }
        drawFrame();
        dirty_ = false;
    }
}

void WatchView::onHostResized()
{
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

void WatchView::onTimer(int timerId)
{
    if (timerId != kPollTimer) {
        return;
    }
    const SessionSnapshot latest = worker_.snapshot();
    if (latest.version != seen_version_) {
        seen_version_ = latest.version;
        snapshot_ = latest;
        dirty_ = true;
    }
    const long long second = static_cast<long long>(std::time(nullptr));
    if (second != last_second_) {
        last_second_ = second;
        dirty_ = true;
    }
}

void WatchView::shutdown()
{
    running_ = false;
    save_settings({page_});
    worker_.shutdown();
}

void WatchView::addButton(Canvas &canvas, float x, float y, float w, float h,
                          const std::string &label, Action action, int page,
                          bool active)
{
    canvas.button(x, y, w, h, label,
                  active ? col::accent : col::btnIdle, col::text, 7.0f);
    hits_.push_back({{x, y, w, h}, action, page});
}

void WatchView::drawHeader(Canvas &canvas)
{
    const float w = canvas.w();
    canvas.rect(0, 0, w, 62, col::panel);
    canvas.text("wristctl", 20, 12, kTitle, col::text);

    const std::string status = snapshot_.status;
    canvas.textRight(status, w - 20, 20, kSmall, statusColor(snapshot_));
    if (!snapshot_.error.empty()) {
        canvas.textRight(snapshot_.error, w - 20, 40, kSmall, col::red);
    } else if (snapshot_.connected) {
        canvas.textRight(snapshot_.model + "  " + snapshot_.address, w - 20, 40,
                         kSmall, col::dim);
    }
}

void WatchView::drawTabs(Canvas &canvas)
{
    const float w = canvas.w();
    const float gap = 6.0f;
    const float buttonW = (w - 40.0f - gap * 5.0f) / 6.0f;
    float x = 20.0f;
    for (int i = 0; i < 6; i++) {
        addButton(canvas, x, 74, buttonW, 34, kPageNames[i], Action::Page, i,
                  page_ == i);
        x += buttonW + gap;
    }
}

void WatchView::drawActions(Canvas &canvas, float y)
{
    float x = 20.0f;
    if (snapshot_.connected) {
        addButton(canvas, x, y, 110, 32, "Disconnect", Action::Disconnect, 0,
                  false);
        x += 118;
        addButton(canvas, x, y, 90, 32, "Refresh", Action::Refresh, 0, false);
        x += 98;
        addButton(canvas, x, y, 100, 32, "Sync time", Action::SyncTime, 0,
                  false);
    } else {
        addButton(canvas, x, y, 110, 32,
                  snapshot_.busy ? "Scanning..." : "Connect", Action::Connect,
                  0, false);
    }
}

void WatchView::drawWatchPage(Canvas &canvas)
{
    float y = 176.0f;
    const float x = 30.0f;
    const float line = 28.0f;
    auto row = [&](const char *label, const std::string &value,
                   Color color = col::text) {
        canvas.text(label, x, y, kSmall, col::dim);
        canvas.text(value, x + 150, y, kText, color);
        y += line;
    };

    row("name", snapshot_.name.empty() ? "-" : snapshot_.name);
    row("model", snapshot_.model.empty() ? "-" : snapshot_.model);
    row("address", snapshot_.address.empty() ? "-" : snapshot_.address);
    row("pairing", snapshot_.pairing.empty() ? "-" : snapshot_.pairing);
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
        row("settings", std::string(flags) + (snapshot_.time_24h ? "  24h" : "  12h"));
    } else {
        row("settings", "not read yet");
    }

    if (snapshot_.demo) {
        canvas.text("Demo data from the recorded session; press Connect for a "
                    "real watch.",
                    x, y + 8, kSmall, col::dim);
    }
}

void WatchView::drawClockPage(Canvas &canvas)
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char clock[32];
    char date[64];
    std::strftime(clock, sizeof clock, "%H:%M:%S", &local);
    std::strftime(date, sizeof date, "%A %d %B %Y", &local);
    canvas.text(clock, 30, 180, 86, col::text);
    canvas.text(date, 34, 286, 22, col::dim);
    canvas.text("host local time", 34, 318, kSmall, col::dim);
    canvas.text("The watch stores no readback for 0x09; verify the display after "
                "a sync.",
                34, 366, kSmall, col::dim);
}

void WatchView::drawAlarmsPage(Canvas &canvas)
{
    canvas.text("Alarms", 30, 176, 26, col::text);
    float y = 218.0f;
    if (!snapshot_.has_alarms) {
        canvas.text("not read yet; connect and press Refresh", 30, y, kText,
                    col::dim);
        return;
    }
    std::string text = snapshot_.alarms;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        canvas.text(text.substr(start, end - start), 30, y, kText, col::text);
        y += 28;
        if (end == text.size()) {
            break;
        }
        start = end + 1;
    }
}

void WatchView::drawTimerPage(Canvas &canvas)
{
    canvas.text("Program timer", 30, 176, 26, col::text);
    float y = 218.0f;
    if (!snapshot_.has_timer) {
        canvas.text("not read yet; connect and press Refresh", 30, y, kText,
                    col::dim);
        return;
    }
    canvas.text(snapshot_.timer, 30, y, kText, col::text);
    y += 36;
    canvas.text("Writes replace the whole program timer and are destructive; "
                "the C++ GUI keeps them read-only for now.",
                30, y, kSmall, col::dim);
}

void WatchView::drawNotifyPage(Canvas &canvas)
{
    canvas.text("Notifications", 30, 176, 26, col::text);
    canvas.text("Class 0x30 is a variable-length, XOR-obfuscated payload. The "
                "Python prototype encodes it;",
                30, 218, kSmall, col::text);
    canvas.text("the C core does not expose an encoder yet, so this page is "
                "read-only in this build.",
                30, 242, kSmall, col::text);
    canvas.text("The Android app and gshockctl remain the write paths for "
                "notifications.",
                30, 266, kSmall, col::dim);
}

void WatchView::drawConsolePage(Canvas &canvas)
{
    canvas.text("Console", 30, 176, 26, col::text);
    float y = 216.0f;
    const auto &log = snapshot_.log;
    const size_t limit = 16;
    const size_t start = log.size() > limit ? log.size() - limit : 0;
    for (size_t i = start; i < log.size(); i++) {
        canvas.text(log[i], 30, y, kSmall, col::dim);
        y += 20;
    }
}

void WatchView::drawFooter(Canvas &canvas)
{
    const float h = canvas.h();
    canvas.rect(0, h - 34, canvas.w(), 34, col::panel);
    const std::string hint =
        "1-6 pages  C connect  R refresh  T sync time  Esc quit";
    canvas.text(hint, 20, h - 30, kSmall, col::dim);
}

void WatchView::handleClick(float x, float y)
{
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (!it->rect.contains(x, y)) {
            continue;
        }
        switch (it->action) {
        case Action::Connect:
            worker_.connect();
            break;
        case Action::Disconnect:
            worker_.disconnect();
            break;
        case Action::Refresh:
            worker_.refresh();
            break;
        case Action::SyncTime:
            worker_.sync_time();
            break;
        case Action::Page:
            page_ = it->page;
            save_settings({page_});
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
    const std::vector<int> sizes = {10, 11, 12, 14, 15, 16, 18, 20,
                                    22, 24, 26, 28, 32, 36, 86};
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

    Canvas canvas(curves_, width, height, nullptr, 0, 0, 0, 0);
    canvas.useShapes(&shapes_);
    if (font_) {
        canvas.useMsdf(font_.get(), &textQuads_);
    }
    canvas.clear(col::bg);

    drawHeader(canvas);
    drawTabs(canvas);
    drawActions(canvas, 120.0f);

    switch (page_) {
    case 0:
        drawWatchPage(canvas);
        break;
    case 1:
        drawClockPage(canvas);
        break;
    case 2:
        drawAlarmsPage(canvas);
        break;
    case 3:
        drawTimerPage(canvas);
        break;
    case 4:
        drawNotifyPage(canvas);
        break;
    case 5:
        drawConsolePage(canvas);
        break;
    default:
        break;
    }
    drawFooter(canvas);

    renderer_->draw(curves_, 0, {}, {}, textQuads_, shapes_);

    if (input().pointerWentDown) {
        handleClick(input().pointerX, input().pointerY);
    }
    for (int digit = 0; digit < 6; digit++) {
        if (input().keyWentDown(0x31 + digit)) {
            page_ = digit;
            save_settings({page_});
            dirty_ = true;
        }
    }
    if (input().keyWentDown(0x43)) { // C
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
