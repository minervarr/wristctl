#include "ble_worker.hh"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>

#include "wristctl/bluez.h"
#include "wristctl/session.h"

namespace wristctl::gui {
namespace {

std::string pairing_name(wc_pairing pairing)
{
    return wc_pairing_name(pairing);
}

void append_log(SessionSnapshot &snapshot, const std::string &line)
{
    snapshot.log.push_back(line);
    if (snapshot.log.size() > 60) {
        snapshot.log.erase(snapshot.log.begin());
    }
}

} // namespace

BleWorker::BleWorker()
{
    snapshot_.demo = true;
    snapshot_.status = "demo session (recorded GBX-100)";
    snapshot_.name = "CASIO GBX-100";
    snapshot_.model = "GBX_100";
    snapshot_.address = "DA:02:44:21:46:51";
    snapshot_.pairing = "bond_requested";
    snapshot_.has_condition = true;
    snapshot_.battery = 3;
    snapshot_.temperature = 30;
    snapshot_.mtu = 442;
    snapshot_.has_settings = true;
    snapshot_.settings_flags = 0x44;
    snapshot_.time_24h = false;
    snapshot_.has_alarms = true;
    snapshot_.alarms = "alarm 1  06:30  on\nalarm 2  07:15  off\nalarm 3  08:00  off";
    snapshot_.has_timer = true;
    snapshot_.timer = "repeat 1  |  00:25  |  00:05";
    snapshot_.log.push_back("demo transport: recorded handshake, no watch");
    thread_ = std::thread(&BleWorker::run, this);
}

BleWorker::~BleWorker()
{
    shutdown();
}

void BleWorker::connect()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.busy) {
            return;
        }
        snapshot_.busy = true;
        snapshot_.status = "connecting...";
        append_log(snapshot_, "scanning for the watch (press a watch button)");
        snapshot_.version++;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({CommandKind::Connect});
    }
    condition_.notify_one();
}

void BleWorker::refresh()
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back({CommandKind::Refresh});
    condition_.notify_one();
}

void BleWorker::sync_time()
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back({CommandKind::SyncTime});
    condition_.notify_one();
}

void BleWorker::disconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back({CommandKind::Disconnect});
    condition_.notify_one();
}

void BleWorker::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    condition_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    cleanup();
}

SessionSnapshot BleWorker::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void BleWorker::run()
{
    for (;;) {
        Command command{CommandKind::Shutdown};
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] {
                return !running_ || !queue_.empty();
            });
            if (!running_) {
                break;
            }
            command = queue_.front();
            queue_.pop_front();
        }
        if (command.kind == CommandKind::Shutdown) {
            break;
        }
        handle(command.kind);
    }
}

void BleWorker::handle(CommandKind kind)
{
    switch (kind) {
    case CommandKind::Connect:
        do_connect();
        break;
    case CommandKind::Refresh:
        do_refresh();
        break;
    case CommandKind::SyncTime:
        do_sync_time();
        break;
    case CommandKind::Disconnect:
        do_disconnect();
        break;
    case CommandKind::Shutdown:
        break;
    }
}

void BleWorker::do_connect()
{
    if (session_ != nullptr) {
        note("already connected");
        return;
    }

    bluez_ = wc_bluez_new();
    if (bluez_ == nullptr) {
        fail("BlueZ unavailable (is bluetoothd running?)");
        return;
    }

    int rc = wc_bluez_connect(bluez_, nullptr, nullptr, 12000);
    if (rc != WC_SESSION_OK) {
        note(std::string("scan: ") + wc_bluez_error(bluez_));
        note("retrying for 20 s; press a watch button");
        rc = wc_bluez_connect(bluez_, nullptr, nullptr, 20000);
    }
    if (rc != WC_SESSION_OK) {
        fail(std::string("not found: ") + wc_bluez_error(bluez_));
        wc_bluez_free(bluez_);
        bluez_ = nullptr;
        return;
    }

    uint8_t app_id[10];
    std::random_device device;
    for (uint8_t &byte : app_id) {
        byte = static_cast<uint8_t>(device());
    }

    session_ = wc_session_new(wc_bluez_transport(bluez_), app_id, 5000);
    if (session_ == nullptr) {
        fail("could not allocate the session");
        wc_bluez_free(bluez_);
        bluez_ = nullptr;
        return;
    }

    wc_identity identity;
    rc = wc_session_handshake(session_, &identity, true, 20000);
    if (rc != WC_SESSION_OK) {
        wc_error error{};
        std::string detail = "handshake failed";
        if (wc_session_last_error(session_, &error)) {
            char buffer[96];
            std::snprintf(buffer, sizeof buffer,
                          "watch rejected class 0x%02x (status 0x%02x)",
                          error.class_id, error.status);
            detail = buffer;
        }
        wc_session_free(session_);
        session_ = nullptr;
        wc_bluez_free(bluez_);
        bluez_ = nullptr;
        fail(detail);
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.demo = false;
    snapshot_.connected = true;
    snapshot_.busy = false;
    snapshot_.name = identity.name;
    snapshot_.model = identity.model != nullptr ? identity.model->name : "unknown";
    snapshot_.address = wc_bluez_address(bluez_);
    snapshot_.pairing = pairing_name(identity.pairing);
    snapshot_.has_condition = identity.has_condition;
    if (identity.has_condition) {
        snapshot_.battery = identity.condition.battery;
        snapshot_.temperature = identity.condition.temperature;
        snapshot_.mtu = identity.condition.mtu;
    }
    snapshot_.status = "connected: " + snapshot_.name;
    snapshot_.error.clear();
    append_log(snapshot_, "handshake complete; pairing=" + snapshot_.pairing);
    snapshot_.version++;
}

void BleWorker::do_refresh()
{
    if (session_ == nullptr) {
        note("refresh ignored: not connected");
        return;
    }

    wc_basic_setting settings{};
    bool have_settings = wc_session_read_basic_settings(session_, &settings) ==
                         WC_SESSION_OK;
    wc_alarm_first first{};
    wc_alarm_rest rest{};
    bool have_alarms = wc_session_read_alarms(session_, &first, &rest) ==
                       WC_SESSION_OK;
    wc_program_timer timer{};
    bool have_timer = wc_session_read_program_timer(session_, &timer) ==
                      WC_SESSION_OK;

    std::lock_guard<std::mutex> lock(mutex_);
    if (have_settings) {
        snapshot_.has_settings = true;
        snapshot_.settings_flags = settings.flags;
        snapshot_.time_24h = settings.time_format_24h != 0;
    }
    if (have_alarms) {
        snapshot_.has_alarms = true;
        snapshot_.alarms.clear();
        char line[96];
        std::snprintf(line, sizeof line, "alarm 1  %02u:%02u  %s",
                      first.hour, first.minute,
                      first.enabled ? "on" : "off");
        snapshot_.alarms = line;
        for (int i = 0; i < 4; i++) {
            std::snprintf(line, sizeof line, "\nalarm %d  %02u:%02u  %s",
                          i + 2, rest.alarms[i].hour, rest.alarms[i].minute,
                          rest.alarms[i].enabled ? "on" : "off");
            snapshot_.alarms += line;
        }
    }
    if (have_timer) {
        snapshot_.has_timer = true;
        char line[128];
        std::snprintf(line, sizeof line, "repeat %u", timer.repeat);
        snapshot_.timer = line;
        for (const auto &workout : timer.workouts) {
            if (workout.minutes == 0 && workout.seconds == 0) {
                continue;
            }
            std::snprintf(line, sizeof line, "  |  %02u:%02u",
                          workout.minutes, workout.seconds);
            snapshot_.timer += line;
        }
    }
    snapshot_.error.clear();
    if (!have_settings) {
        snapshot_.error = "some reads failed (0x13/0x15/0x16/0x2A)";
    }
    append_log(snapshot_, "read settings, alarms and program timer");
    snapshot_.version++;
}

void BleWorker::do_sync_time()
{
    if (session_ == nullptr) {
        note("sync ignored: not connected");
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            now.time_since_epoch())
                            .count() %
                        1000000;
    std::tm local{};
    localtime_r(&seconds, &local);

    wc_current_time value{};
    value.year = static_cast<uint16_t>(local.tm_year + 1900);
    value.month = static_cast<uint8_t>(local.tm_mon + 1);
    value.day = static_cast<uint8_t>(local.tm_mday);
    value.hour = static_cast<uint8_t>(local.tm_hour);
    value.minute = static_cast<uint8_t>(local.tm_min);
    value.second = static_cast<uint8_t>(local.tm_sec);
    value.weekday = static_cast<uint8_t>(local.tm_wday);
    value.nano = static_cast<uint8_t>((micros * 256 / 1000000) & 0xFF);

    uint8_t frame[WC_LEN_CURRENT_TIME];
    size_t length = 0;
    if (wc_current_time_encode(&value, frame, sizeof frame, &length) != 0) {
        fail("could not encode the time frame");
        return;
    }
    if (wc_session_write_all(session_, frame, length) != WC_SESSION_OK) {
        fail("time write failed");
        return;
    }
    note("time written; check the watch display (0x09 cannot be read back)");
}

void BleWorker::do_disconnect()
{
    if (session_ != nullptr) {
        wc_session_free(session_);
        session_ = nullptr;
    }
    if (bluez_ != nullptr) {
        wc_bluez_free(bluez_);
        bluez_ = nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.connected = false;
    snapshot_.busy = false;
    snapshot_.status = "disconnected";
    append_log(snapshot_, "disconnected");
    snapshot_.version++;
}

void BleWorker::cleanup()
{
    if (session_ != nullptr) {
        wc_session_free(session_);
        session_ = nullptr;
    }
    if (bluez_ != nullptr) {
        wc_bluez_free(bluez_);
        bluez_ = nullptr;
    }
}

void BleWorker::note(const std::string &line)
{
    std::lock_guard<std::mutex> lock(mutex_);
    append_log(snapshot_, line);
    snapshot_.version++;
}

void BleWorker::fail(const std::string &line)
{
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.connected = false;
    snapshot_.busy = false;
    snapshot_.status = "error";
    snapshot_.error = line;
    append_log(snapshot_, line);
    snapshot_.version++;
}

} // namespace wristctl::gui
