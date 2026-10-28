#pragma once

// The GUI's BLE worker: owns the BlueZ transport and the C session on one
// thread, so the render thread never blocks on D-Bus or the watch.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct wc_bluez;
struct wc_session;

namespace wristctl::gui {

struct SessionSnapshot {
    bool connected = false;
    bool busy = false;
    bool demo = false;
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
    unsigned settings_flags = 0;
    bool time_24h = false;
    bool has_alarms = false;
    std::string alarms;
    bool has_timer = false;
    std::string timer;
    std::vector<std::string> log;
    unsigned version = 0;
};

class BleWorker {
public:
    BleWorker();
    ~BleWorker();

    BleWorker(const BleWorker &) = delete;
    BleWorker &operator=(const BleWorker &) = delete;

    void connect();
    void refresh();
    void sync_time();
    void disconnect();
    void shutdown();

    SessionSnapshot snapshot() const;

private:
    enum class CommandKind { Connect, Refresh, SyncTime, Disconnect, Shutdown };
    struct Command {
        CommandKind kind;
    };

    void run();
    void handle(CommandKind kind);
    void do_connect();
    void do_refresh();
    void do_sync_time();
    void do_disconnect();
    void cleanup();

    void post(const std::string &status, bool connected, bool busy);
    void note(const std::string &line);
    void fail(const std::string &line);
    void touch();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Command> queue_;
    bool running_ = true;
    std::thread thread_;

    SessionSnapshot snapshot_;
    wc_bluez *bluez_ = nullptr;
    wc_session *session_ = nullptr;
};

} // namespace wristctl::gui
