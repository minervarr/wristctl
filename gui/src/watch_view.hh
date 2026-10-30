#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app_view.hh"
#include "ble_worker.hh"
#include "canvas.hh"
#include "demo_scenarios.hh"
#include "frame_input_view.hh"
#include "settings.hh"

class Host;
class Renderer;
class RasterFont;

namespace wristctl::gui {

class WatchView : public FrameInputView {
public:
    explicit WatchView(bool auto_connect = true, bool demo = false);
    ~WatchView() override;

    // A platform with no ambient host (Android) injects one; desktop calls
    // create() and gets make_host(). Two overloads because a defaulted
    // unique_ptr<Host> argument needs Host's destructor at the call site.
    bool create();
    bool create(std::unique_ptr<Host> injected_host);
    void run();
    void connect_now();

    // Android has no AppShellActivity here, so Host::displayDpi() is 0. The
    // entry point reads the configuration's density and hands it over before
    // create(). Desktop leaves the 1.0 default.
    void set_display_density(float density) { display_density_ = density; }
    void set_system_bottom_inset(int px) { system_bottom_inset_ = px; }

    void onHostResized() override;
    void onHostLayoutInvalidated() override;
    void onHostExposed() override;
    void onAppForegrounded() override;
    void onTimer(int timerId) override;
    void onMouseWheel(int x, int y, int delta) override;
    void shutdown() override;

    // Android destroys the surface when the app is backgrounded and hands back
    // a different one on return. The swapchain and every GPU object go with it.
    void onSurfaceLost() override;
    bool onSurfaceRecreated() override;

private:
    enum class Action {
        Connect,
        Disconnect,
        Refresh,
        SyncTime,
        ToggleTimeSource,
        ToggleSyncOnConnect,
        Page,
        Scan,
        StopScan,
        ConnectSelected,
        SelectDevice,
        ToggleAll,
        Grant,
        OpenSettings,
        TurnOnBluetooth,
        OpenLocation,
        ToggleAutoConnect,
        AlarmToggle,
        AlarmMode,
        AlarmHour,
        AlarmMinute,
        AlarmChime,
        AlarmApply,
        AlarmRevert,
        TimerRepeat,
        TimerMinutes,
        TimerSeconds,
        TimerBeep,
        TimerInitialize,
        TimerApply,
        TimerRevert,
        ClockUnit,
        CityTimezone,
        CityDstDiff,
        CityDstToggle,
        CityApply,
        CityRevert,
        ToggleHomeCard,
        ToggleAppTimeFormat,
        SettingToggle,
        SettingCycle,
        SettingsApply,
        SettingsRevert,
        EditField,
        NotifyType,
        NotifySend,
        ToggleNotifyForward,
        OpenNotificationAccess,
        NotifyQuietHours,
        StopPhoneFinder,
        DemoScenario,
        EditorKey,
        EditorBackspace,
        EditorSpace,
        EditorDone,
        EditorCancel
    };

    struct Hit {
        Rect rect;
        Action action;
        int page;
        std::string address;
        int arg = 0;
        int arg2 = 0;
    };

    struct Layout {
        float left = 0, top = 0;
        float width = 0, height = 0;
        float margin = 20;
        float header_h = 62;
        float title_size = 28, small_size = 14, text_size = 18;
        float page_title_size = 26, clock_size = 86, date_size = 22;
        float title_y = 12, status_y = 20, detail_y = 40;
        bool stacked_header = false;
        float tabs_y = 74, tab_h = 34, tab_w = 0, tab_gap = 6;
        int tab_cols = 6, tab_rows = 1;
        float actions_y = 120, action_h = 32;
        float content_x = 30, content_y = 176;
        float content_w = 0, content_bottom = 0;
        float footer_h = 34;
        float row_line = 28, label_w = 150;
        float scroll_step = 40;
    };

    Layout make_layout(const Canvas &canvas) const;

    void drawFrame();
    void drawHeader(Canvas &canvas, const Layout &layout);
    void drawTabs(Canvas &canvas, const Layout &layout);
    float drawSectionTitle(Canvas &canvas, const Layout &layout,
                           const std::string &title, float x, float y);
    float drawKeyValue(Canvas &canvas, const Layout &layout,
                       const std::string &label, const std::string &value,
                       float x, float y, Color color = col::text);
    float drawNote(Canvas &canvas, const Layout &layout,
                   const std::string &text, float x, float y,
                   Color color = col::dim);
    float drawOverviewPage(Canvas &canvas, const Layout &layout);
    float drawWatchPage(Canvas &canvas, const Layout &layout);
    float drawPairPage(Canvas &canvas, const Layout &layout);
    float drawClockPage(Canvas &canvas, const Layout &layout);
    float drawAlarmsPage(Canvas &canvas, const Layout &layout);
    float drawTimerPage(Canvas &canvas, const Layout &layout);
    float drawNotifyPage(Canvas &canvas, const Layout &layout);
    float drawSettingsPage(Canvas &canvas, const Layout &layout);
    float drawDiagnosticsPage(Canvas &canvas, const Layout &layout);
    void drawFooter(Canvas &canvas, const Layout &layout);
    void drawEditor(Canvas &canvas, const Layout &layout);
    void ensureFont();
    void handleInput();
    void handleClick(float x, float y);
    void addButton(Canvas &canvas, float x, float y, float w, float h,
                   const std::string &label, Action action, int page,
                   bool active, float radius, int arg = 0, int arg2 = 0);
    void addHit(const Rect &rect, Action action, const std::string &address = {},
                int arg = 0, int arg2 = 0);
    bool selected_is_bonded() const;
    float drawWrapped(Canvas &canvas, const std::string &text, float x,
                      float y, float max_w, float size, Color color,
                      float line_h) const;
    std::string truncateToWidth(Canvas &canvas, const std::string &text,
                                float max_w, float size) const;
    float page_scroll(int page) const { return scroll_[page]; }
    void clamp_scroll(int page, float page_height, float viewport_h);

    enum class Detail { None, City, Alarm, TimerStep, NotificationTest };
    void open_detail(Detail detail, int index = 0);
    void close_detail();

    // Draft editors. The worker publishes read state; these copies only change
    // when the user presses a control, and a refresh never clobbers a draft
    // that has not been written.
    void sync_drafts();
    void open_editor(int field);
    void close_editor(bool apply);
    std::string *editor_field();
    void adjust_alarm(int index, int hour_delta, int minute_delta);
    void adjust_timer(int index, int field, int delta);
    void draw_editor_key(Canvas &canvas, float x, float y, float w, float h,
                         const std::string &label, int key, Action action);

    std::unique_ptr<Host> host_;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<RasterFont> font_;
    BleWorker worker_;
    SessionSnapshot snapshot_;

    std::vector<float> curves_;
    std::vector<float> shapes_;
    std::vector<float> textQuads_;
    std::vector<Hit> hits_;

    Route route_ = Route::Overview;
    Detail detail_ = Detail::None;
    int detail_index_ = 0;
    DemoScenario demo_scenario_ = DemoScenario::Recorded;
    bool running_ = true;
    bool dirty_ = true;
    bool touch_ = false;
    bool surface_only_ = false;
    bool show_all_ = false;
    bool auto_connect_arg_ = true;
    GuiSettings settings_;
    std::string selected_address_;
    float display_density_ = 1.0f;
    int system_bottom_inset_ = 0;
    unsigned seen_version_ = 0;
    long long last_second_ = 0;
    int width_ = 0;
    int height_ = 0;
    int last_page_height_ = 0;
    int last_viewport_h_ = 0;
    static constexpr int kRouteCount = static_cast<int>(Route::Count);
    float scroll_[kRouteCount] = {};

    // Alarm/timer/settings drafts. `*_dirty` means the user changed something
    // since the last published read; `*_confirm` is the second-tap guard for a
    // write that replaces watch state.
    bool alarms_dirty_ = false;
    bool alarms_confirm_ = false;
    wc_alarm_first edit_first_{};
    wc_alarm_rest edit_rest_{};
    bool timer_dirty_ = false;
    bool timer_confirm_ = false;
    wc_program_timer edit_timer_{};
    bool clock_dirty_ = false;
    bool clock_confirm_ = false;
    int clock_unit_ = 0;
    wc_dst_watch_state edit_dst_state_{};
    wc_dst_setting edit_dst_settings_[5]{};
    wc_city_name edit_city_names_[5]{};
    std::string edit_city_name_;
    bool settings_dirty_ = false;
    wc_basic_setting edit_settings_{};
    std::string edit_name_;
    bool name_dirty_ = false;
    std::string ntp_server_text_;

    // Notification composer. Type values follow the vendor app's category
    // byte; 0 is the generic/other channel.
    int notify_type_ = 0;
    std::string notify_app_ = "wristctl";
    std::string notify_title_ = "Test notification";
    std::string notify_text_ = "Hello from wristctl";
    std::string notify_packages_text_;

    enum EditorField {
        EDIT_NONE = 0,
        EDIT_WATCH_NAME,
        EDIT_NOTIFY_APP,
        EDIT_NOTIFY_TITLE,
        EDIT_NOTIFY_TEXT,
        EDIT_NTP_SERVERS,
        EDIT_CITY_NAME,
        EDIT_NOTIFY_PACKAGES
    };
    bool editor_open_ = false;
    bool editor_shift_ = false;
    int editor_field_ = EDIT_NONE;
    std::string editor_text_;
    size_t editor_limit_ = 19;
};

} // namespace wristctl::gui
