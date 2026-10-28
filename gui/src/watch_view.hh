#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app_view.hh"
#include "ble_worker.hh"
#include "canvas.hh"
#include "frame_input_view.hh"

class Host;
class Renderer;
class RasterFont;

namespace wristctl::gui {

class WatchView : public FrameInputView {
public:
    WatchView();
    ~WatchView() override;

    bool create();
    void run();
    void connect_now();

    void onHostResized() override;
    void onHostLayoutInvalidated() override;
    void onHostExposed() override;
    void onTimer(int timerId) override;
    void shutdown() override;

private:
    enum class Action { Connect, Disconnect, Refresh, SyncTime, Page };

    struct Hit {
        Rect rect;
        Action action;
        int page;
    };

    void drawFrame();
    void drawHeader(Canvas &canvas);
    void drawTabs(Canvas &canvas);
    void drawActions(Canvas &canvas, float y);
    void drawWatchPage(Canvas &canvas);
    void drawClockPage(Canvas &canvas);
    void drawAlarmsPage(Canvas &canvas);
    void drawTimerPage(Canvas &canvas);
    void drawNotifyPage(Canvas &canvas);
    void drawConsolePage(Canvas &canvas);
    void drawFooter(Canvas &canvas);
    void ensureFont();
    void handleClick(float x, float y);
    void addButton(Canvas &canvas, float x, float y, float w, float h,
                   const std::string &label, Action action, int page,
                   bool active);

    std::unique_ptr<Host> host_;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<RasterFont> font_;
    BleWorker worker_;
    SessionSnapshot snapshot_;

    std::vector<float> curves_;
    std::vector<float> shapes_;
    std::vector<float> textQuads_;
    std::vector<Hit> hits_;

    int page_ = 0;
    bool running_ = true;
    bool dirty_ = true;
    unsigned seen_version_ = 0;
    long long last_second_ = 0;
    int width_ = 0;
    int height_ = 0;
};

} // namespace wristctl::gui
