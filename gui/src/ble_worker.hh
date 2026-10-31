#pragma once

// The GUI's BLE worker: owns the platform transport and the C session on one
// thread, so the render thread never blocks on D-Bus, JNI or the watch.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "phone_model.hh"
#include "reconnect_policy.hh"
#include "watch_link.hh"

struct wc_session;

namespace wristctl::gui {

struct SntpSample;

struct TimeConfig {
    bool use_ntp = false;
    bool sync_on_connect = false;
    std::vector<std::string> servers;
    int timeout_ms = 2000;
};

struct SessionSnapshot {
    bool connected = false;
    bool busy = false;
    bool demo = false;
    bool auto_connect = false;
    std::string status = "not connected";
    std::string name;
    std::string model;
    std::string address;
    std::string pairing;
    std::string error;
    bool has_condition = false;
    int battery = 0;
    int temperature = 0;
    int mtu = 0;
    bool has_settings = false;
    wc_basic_setting settings{};
    unsigned settings_flags = 0;
    bool time_24h = false;
    bool has_alarms = false;
    wc_alarm_first alarm_first{};
    wc_alarm_rest alarm_rest{};
    bool has_timer = false;
    wc_program_timer timer_value{};
    wc_frame module_frame{};
    wc_frame version_frame{};
    bool has_clock = false;
    bool has_utc_clock = false;
    wc_dst_watch_state dst_state{};
    wc_dst_setting dst_settings[5]{};
    wc_city_name city_names[5]{};
    TimeConfig time_config;
    PhoneConfig phone_config;
    bool notification_access = false;
    unsigned notifications_forwarded = 0;
    unsigned notifications_filtered = 0;
    bool has_time_sample = false;
    std::chrono::system_clock::time_point time_corrected_at{};
    std::chrono::steady_clock::time_point time_sampled_at{};
    std::string time_server;
    std::string time_error;
    double time_offset_ms = 0;
    double time_round_trip_ms = 0;
    std::vector<std::string> log;
    unsigned version = 0;

    // Pairing UI. `devices` is all named scan results; the view filters.
    std::vector<WatchDevice> devices;
    bool scanning = false;
    bool supports_scan = false;
    bool permission_missing = false;
    bool bluetooth_off = false;
    bool location_off = false;
    std::string selected_address;
};

class BleWorker {
public:
    explicit BleWorker(bool auto_connect = true, bool demo = false);
    ~BleWorker();

    BleWorker(const BleWorker &) = delete;
    BleWorker &operator=(const BleWorker &) = delete;

    void start();
    void set_auto_connect(bool on);
    void set_time_config(const TimeConfig &config);
    void set_phone_config(const PhoneConfig &config);
    void open_notification_settings();
    void stop_phone_finder();
    void connect();
    void scan();
    void stop_scan();
    void connect_to(const std::string &address);
    void grant_permissions();
    void open_settings();
    void open_location_settings();
    void enable_bluetooth();
    void refresh();
    void sync_time();
    void disconnect();

    /* Write paths. The editor owns the draft; the worker only encodes and
     * sends it. Each call sets busy immediately and publishes the result in
     * the next snapshot. Program-timer writes replace every entry, so the
     * caller must have read the current value and confirmed the change. */
    void write_alarms(const wc_alarm_first &first, const wc_alarm_rest &rest);
    void write_timer(const wc_program_timer &timer);
    void write_city(uint8_t unit, const wc_dst_watch_state &state,
                    const wc_dst_setting &setting, const wc_city_name &name);
    void write_settings(const wc_basic_setting &settings,
                        const std::string &name = std::string());
    void rename_watch(const std::string &name);
    void send_notification(uint8_t type, const std::string &app,
                           const std::string &title, const std::string &text);

    void shutdown();

    SessionSnapshot snapshot() const;

private:
    using Clock = ReconnectPolicy::Clock;

    enum class CommandKind {
        Connect,
        Scan,
        StopScan,
        GrantPermissions,
        OpenSettings,
        OpenLocationSettings,
        EnableBluetooth,
        OpenNotificationSettings,
        StopPhoneFinder,
        ApplyAutoConnect,
        Refresh,
        SyncTime,
        WriteAlarms,
        WriteTimer,
        WriteCity,
        WriteSettings,
        RenameWatch,
        SendNotification,
        Disconnect,
        Shutdown
    };
    struct AlarmWrite {
        wc_alarm_first first;
        wc_alarm_rest rest;
    };
    struct CityWrite {
        uint8_t unit;
        wc_dst_watch_state state;
        wc_dst_setting setting;
        wc_city_name name;
    };
    struct SettingsWrite {
        wc_basic_setting settings;
        bool has_name = false;
        char name[20] = {};
    };
    struct NameWrite {
        char name[20];
    };
    struct NotificationWrite {
        uint8_t type;
        std::string app;
        std::string title;
        std::string text;
        bool automatic = false;
    };
    using CommandPayload = std::variant<std::monostate, AlarmWrite,
                                        wc_program_timer, CityWrite, SettingsWrite,
                                        NameWrite, NotificationWrite>;
    struct Command {
        Command(CommandKind command_kind,
                CommandPayload command_payload = std::monostate{})
            : kind(command_kind), payload(std::move(command_payload))
        {
        }
        CommandKind kind;
        CommandPayload payload;
    };

    void run();
    void handle(const Command &command);
    void tick();
    void poll_scan();
    void attempt_connect();
    void schedule_retry(const std::string &why);
    void handle_link_loss();
    void heartbeat();
    void set_status(const std::string &status);
    void do_connect();
    void do_scan();
    void do_stop_scan();
    void do_grant_permissions();
    void do_open_settings();
    void do_open_location_settings();
    void do_enable_bluetooth();
    void do_open_notification_settings();
    void do_stop_phone_finder();
    void do_apply_auto_connect();
    void forward_phone_notification();
    void do_refresh();
    void do_sync_time(const SntpSample *prefetched = nullptr);
    void do_write_alarms(const AlarmWrite &write);
    void do_write_timer(const wc_program_timer &timer);
    void do_write_city(const CityWrite &write);
    void do_write_settings(const SettingsWrite &write);
    void do_rename_watch(const NameWrite &write);
    void do_send_notification(const NotificationWrite &write);
    void do_disconnect();
    void cleanup();
    void reset_scan_link();

    // A transient link, used only for a permission/radio/settings action when
    // no scan is running. Returns null when the platform has no link.
    std::unique_ptr<WatchLink> transient_link();

    static bool cancel_cb(void *ctx);
    bool cancel_requested() const;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Command> queue_;
    std::atomic<bool> running_{true};
    std::atomic<bool> cancel_connect_{false};
    std::thread thread_;

    SessionSnapshot snapshot_;
    TimeConfig time_config_;
    std::unique_ptr<WatchLink> link_;
    std::unique_ptr<WatchLink> scan_link_;
    wc_session *session_ = nullptr;
    std::string selected_address_;
    ReconnectPolicy reconnect_;
    Clock::time_point last_heartbeat_{};
    bool start_auto_connect_ = true;
};

} // namespace wristctl::gui
