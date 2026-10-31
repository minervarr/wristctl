#include "ble_worker.hh"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <optional>
#include <random>
#include <variant>

#include "sntp_client.hh"
#include "utc_sync.hh"
#include "watch_link.hh"
#include "wristctl/notify.h"
#include "wristctl/session.h"

namespace wristctl::gui {
namespace {

constexpr uint32_t kScanWindowMs = 10000;
constexpr auto kTickInterval = std::chrono::milliseconds(250);
constexpr auto kHeartbeatInterval = std::chrono::seconds(30);

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

bool same_devices(const std::vector<WatchDevice> &a,
                  const std::vector<WatchDevice> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].address != b[i].address || a[i].name != b[i].name ||
            a[i].rssi != b[i].rssi || a[i].bonded != b[i].bonded) {
            return false;
        }
    }
    return true;
}

wc_current_time time_at(std::chrono::system_clock::time_point now, bool utc)
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           now.time_since_epoch())
                           .count() %
                       1000000000;
    std::tm calendar{};
    if (utc) {
        calendar = utc_tm(now);
    } else {
        // localtime_r() follows the process TZ. gui_main applies --timezone or
        // $WRISTCTL_TIMEZONE before the worker thread starts.
        localtime_r(&seconds, &calendar);
    }

    wc_current_time value{};
    value.year = static_cast<uint16_t>(calendar.tm_year + 1900);
    value.month = static_cast<uint8_t>(calendar.tm_mon + 1);
    value.day = static_cast<uint8_t>(calendar.tm_mday);
    value.hour = static_cast<uint8_t>(calendar.tm_hour);
    value.minute = static_cast<uint8_t>(calendar.tm_min);
    value.second = static_cast<uint8_t>(calendar.tm_sec);
    value.weekday = static_cast<uint8_t>(calendar.tm_wday);
    value.nano = static_cast<uint8_t>((nanos * 256 / 1000000000) & 0xFF);
    return value;
}

wc_current_time local_time_now()
{
    return time_at(std::chrono::system_clock::now(), false);
}

SntpResult query_time_servers(const TimeConfig &config)
{
    if (config.servers.empty()) {
        SntpResult result;
        result.error = "no SNTP servers configured";
        return result;
    }

    SntpResult result;
    std::string errors;
    for (const std::string &server : config.servers) {
        result = query_sntp(
            {server, 123, std::chrono::milliseconds(config.timeout_ms)});
        if (result.ok) {
            return result;
        }
        if (!errors.empty()) {
            errors += "; ";
        }
        errors += server + ": " + result.error;
    }
    result.ok = false;
    result.error = errors;
    return result;
}

} // namespace

BleWorker::BleWorker(bool auto_connect, bool demo)
{
    // Auto-connect is deferred to start(): the Android permission dialogs need
    // a resumed activity, and the worker thread otherwise races the first
    // window. Desktop calls start() from WatchView::create() too.
    reconnect_.set_desired(false, Clock::now());
    start_auto_connect_ = auto_connect;
    snapshot_.auto_connect = auto_connect;
    snapshot_.demo = demo;
    if (!demo) {
        snapshot_.status = "not connected";
    } else {
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
        snapshot_.settings.flags = 0x44;
        snapshot_.settings.time_format_24h = 0;
        snapshot_.settings.auto_light_off = 1;
        snapshot_.settings.language = 0;
        snapshot_.settings_flags = 0x44;
        snapshot_.time_24h = false;
        snapshot_.has_alarms = true;
        memset(&snapshot_.alarm_first, 0, sizeof snapshot_.alarm_first);
        memset(&snapshot_.alarm_rest, 0, sizeof snapshot_.alarm_rest);
        snapshot_.alarm_first.enabled = 1;
        snapshot_.alarm_first.constant = 0x40;
        snapshot_.alarm_first.hour = 6;
        snapshot_.alarm_first.minute = 30;
        snapshot_.alarm_rest.alarms[0].enabled = 1;
        snapshot_.alarm_rest.alarms[0].constant = 0x40;
        snapshot_.alarm_rest.alarms[0].hour = 7;
        snapshot_.alarm_rest.alarms[0].minute = 15;
        snapshot_.alarm_rest.alarms[1].constant = 0x40;
        snapshot_.alarm_rest.alarms[1].hour = 8;
        snapshot_.alarm_rest.alarms[2].constant = 0x40;
        snapshot_.alarm_rest.alarms[2].hour = 9;
        snapshot_.alarm_rest.alarms[3].constant = 0x40;
        snapshot_.alarm_rest.alarms[3].hour = 10;
        snapshot_.has_timer = true;
        memset(&snapshot_.timer_value, 0, sizeof snapshot_.timer_value);
        snapshot_.timer_value.repeat = 1;
        snapshot_.timer_value.workouts[0].minutes = 25;
        snapshot_.timer_value.workouts[1].minutes = 5;
        snapshot_.log.push_back("demo transport: recorded handshake, no watch");
    }
    thread_ = std::thread(&BleWorker::run, this);
}

BleWorker::~BleWorker()
{
    shutdown();
}

void BleWorker::start()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.supports_scan = platform_supports_scan();
        if (start_auto_connect_) {
            // reconnect_ belongs to the worker thread; apply the preference
            // there rather than racing its tick.
            queue_.push_back({CommandKind::ApplyAutoConnect});
        }
    }
    condition_.notify_one();
}

void BleWorker::set_auto_connect(bool on)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        start_auto_connect_ = on;
        snapshot_.auto_connect = on;
        snapshot_.version++;
        queue_.push_back({CommandKind::ApplyAutoConnect});
    }
    condition_.notify_one();
}

void BleWorker::set_time_config(const TimeConfig &config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    time_config_ = config;
    snapshot_.time_config = config;
    snapshot_.version++;
}

void BleWorker::set_phone_config(const PhoneConfig &config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.phone_config = config;
    snapshot_.version++;
}

void BleWorker::open_notification_settings()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({CommandKind::OpenNotificationSettings});
    }
    condition_.notify_one();
}

void BleWorker::stop_phone_finder()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({CommandKind::StopPhoneFinder});
    }
    condition_.notify_one();
}

void BleWorker::connect()
{
    cancel_connect_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.connected) {
            snapshot_.status = "already connected";
            append_log(snapshot_, "already connected");
            snapshot_.version++;
            return;
        }
        // The demo snapshot is not watch state. Clear it when a real scan
        // starts so a failed connect leaves an honest empty page rather than
        // recorded data that looks live.
        snapshot_.busy = true;
        snapshot_.demo = false;
        snapshot_.auto_connect = true;
        snapshot_.error.clear();
        snapshot_.status = "connecting...";
        snapshot_.name.clear();
        snapshot_.model.clear();
        snapshot_.address.clear();
        snapshot_.pairing.clear();
        snapshot_.has_condition = false;
        snapshot_.has_settings = false;
        snapshot_.has_alarms = false;
        snapshot_.has_timer = false;
        snapshot_.has_clock = false;
        snapshot_.has_utc_clock = false;
        memset(&snapshot_.settings, 0, sizeof snapshot_.settings);
        memset(&snapshot_.alarm_first, 0, sizeof snapshot_.alarm_first);
        memset(&snapshot_.alarm_rest, 0, sizeof snapshot_.alarm_rest);
        memset(&snapshot_.timer_value, 0, sizeof snapshot_.timer_value);
        append_log(snapshot_, "auto-connect on; scanning for the watch");
        snapshot_.version++;
        queue_.push_back({CommandKind::Connect});
    }
    condition_.notify_one();
}

void BleWorker::scan()
{
    cancel_connect_.store(true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.connected) {
            snapshot_.status = "disconnect first";
            snapshot_.version++;
            return;
        }
        snapshot_.busy = false;
        snapshot_.demo = false;
        snapshot_.error.clear();
        snapshot_.status = "scanning for watches...";
        snapshot_.devices.clear();
        snapshot_.selected_address.clear();
        snapshot_.scanning = true;
        snapshot_.permission_missing = false;
        snapshot_.bluetooth_off = false;
        snapshot_.location_off = false;
        selected_address_.clear();
        append_log(snapshot_, "scanning for nearby watches");
        snapshot_.version++;
        queue_.push_back({CommandKind::Scan});
    }
    condition_.notify_one();
}

void BleWorker::stop_scan()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.scanning = false;
        snapshot_.status = "scan stopped";
        snapshot_.version++;
        queue_.push_back({CommandKind::StopScan});
    }
    condition_.notify_one();
}

void BleWorker::connect_to(const std::string &address)
{
    if (address.empty()) {
        return;
    }
    cancel_connect_.store(true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.connected) {
            return;
        }
        selected_address_ = address;
        snapshot_.selected_address = address;
        snapshot_.scanning = false;
        snapshot_.busy = true;
        snapshot_.demo = false;
        snapshot_.error.clear();
        snapshot_.status = "connecting to " + address + "...";
        snapshot_.version++;
        queue_.push_back({CommandKind::Connect});
    }
    condition_.notify_one();
}

void BleWorker::grant_permissions()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({CommandKind::GrantPermissions});
    }
    condition_.notify_one();
}

void BleWorker::open_settings()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({CommandKind::OpenSettings});
    }
    condition_.notify_one();
}

void BleWorker::open_location_settings()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({CommandKind::OpenLocationSettings});
    }
    condition_.notify_one();
}

void BleWorker::enable_bluetooth()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back({CommandKind::EnableBluetooth});
    }
    condition_.notify_one();
}

void BleWorker::refresh()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!snapshot_.connected) {
            append_log(snapshot_, "refresh ignored: not connected");
            snapshot_.version++;
            return;
        }
        if (snapshot_.busy) {
            return;
        }
        snapshot_.busy = true;
        snapshot_.status = "reading watch state...";
        snapshot_.version++;
        queue_.push_back({CommandKind::Refresh});
    }
    condition_.notify_one();
}

void BleWorker::sync_time()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!snapshot_.connected && !snapshot_.demo) {
            append_log(snapshot_, "sync ignored: not connected");
            snapshot_.version++;
            return;
        }
        if (snapshot_.busy) {
            return;
        }
        snapshot_.busy = true;
        snapshot_.error.clear();
        snapshot_.status = "writing time...";
        snapshot_.version++;
        queue_.push_back({CommandKind::SyncTime});
    }
    condition_.notify_one();
}

void BleWorker::write_alarms(const wc_alarm_first &first,
                             const wc_alarm_rest &rest)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!snapshot_.connected && !snapshot_.demo) {
            append_log(snapshot_, "alarm write ignored: not connected");
            snapshot_.version++;
            return;
        }
        if (snapshot_.busy) {
            return;
        }
        snapshot_.busy = true;
        snapshot_.error.clear();
        snapshot_.status = "writing alarms...";
        snapshot_.version++;
        queue_.push_back(
            Command{CommandKind::WriteAlarms, AlarmWrite{first, rest}});
    }
    condition_.notify_one();
}

void BleWorker::write_timer(const wc_program_timer &timer)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!snapshot_.connected && !snapshot_.demo) {
            append_log(snapshot_, "timer write ignored: not connected");
            snapshot_.version++;
            return;
        }
        if (snapshot_.busy) {
            return;
        }
        snapshot_.busy = true;
        snapshot_.error.clear();
        snapshot_.status = "writing program timer...";
        snapshot_.version++;
        queue_.push_back(Command{CommandKind::WriteTimer, timer});
    }
    condition_.notify_one();
}

void BleWorker::write_city(uint8_t unit, const wc_dst_watch_state &state,
                           const wc_dst_setting &setting,
                           const wc_city_name &name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if ((!snapshot_.connected && !snapshot_.demo) || snapshot_.busy) {
        return;
    }
    if (unit >= 5 || setting.unit != unit || name.unit != unit) {
        snapshot_.error = "city write has inconsistent unit numbers";
        snapshot_.status = "city not written";
        snapshot_.version++;
        return;
    }
    snapshot_.busy = true;
    snapshot_.error.clear();
    snapshot_.status = "writing city and DST settings...";
    snapshot_.version++;
    queue_.push_back(Command{CommandKind::WriteCity,
                             CityWrite{unit, state, setting, name}});
    condition_.notify_one();
}

void BleWorker::write_settings(const wc_basic_setting &settings,
                               const std::string &name)
{
    SettingsWrite write{};
    write.settings = settings;
    if (!name.empty()) {
        if (name.size() > 19) {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.error = "watch name is longer than 19 bytes";
            snapshot_.status = "settings not written";
            snapshot_.version++;
            return;
        }
        write.has_name = true;
        memcpy(write.name, name.c_str(), name.size());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!snapshot_.connected && !snapshot_.demo) {
            append_log(snapshot_, "settings write ignored: not connected");
            snapshot_.version++;
            return;
        }
        if (snapshot_.busy) {
            return;
        }
        snapshot_.busy = true;
        snapshot_.error.clear();
        snapshot_.status = "writing settings...";
        snapshot_.version++;
        queue_.push_back(Command{CommandKind::WriteSettings, write});
    }
    condition_.notify_one();
}

void BleWorker::rename_watch(const std::string &name)
{
    if (name.size() > 19) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.error = "watch name is longer than 19 bytes";
        snapshot_.status = "name not written";
        snapshot_.version++;
        return;
    }
    NameWrite write{};
    memcpy(write.name, name.c_str(), name.size());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!snapshot_.connected && !snapshot_.demo) {
            append_log(snapshot_, "rename ignored: not connected");
            snapshot_.version++;
            return;
        }
        if (snapshot_.busy) {
            return;
        }
        snapshot_.busy = true;
        snapshot_.error.clear();
        snapshot_.status = "writing watch name...";
        snapshot_.version++;
        queue_.push_back(Command{CommandKind::RenameWatch, write});
    }
    condition_.notify_one();
}

void BleWorker::send_notification(uint8_t type, const std::string &app,
                                  const std::string &title,
                                  const std::string &text)
{
    if (app.size() > WC_NOTIFY_FIELD_MAX ||
        title.size() > WC_NOTIFY_FIELD_MAX ||
        text.size() > WC_NOTIFY_FIELD_MAX) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.error = "notification fields are limited to 255 bytes";
        snapshot_.status = "notification not sent";
        append_log(snapshot_, "notification field too long");
        snapshot_.version++;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!snapshot_.connected && !snapshot_.demo) {
            append_log(snapshot_, "notification ignored: not connected");
            snapshot_.version++;
            return;
        }
        if (snapshot_.busy) {
            return;
        }
        snapshot_.busy = true;
        snapshot_.error.clear();
        snapshot_.status = "sending notification...";
        snapshot_.version++;
        queue_.push_back(Command{CommandKind::SendNotification,
                                 NotificationWrite{type, app, title, text}});
    }
    condition_.notify_one();
}

void BleWorker::disconnect()
{
    // Abort an in-flight scan or read promptly; the queued command below turns
    // auto-connect off once the worker reaches it.
    cancel_connect_.store(true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = true;
        snapshot_.status = "disconnecting...";
        snapshot_.version++;
        queue_.push_back({CommandKind::Disconnect});
    }
    condition_.notify_one();
}

void BleWorker::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load()) {
            return;
        }
        running_.store(false);
        cancel_connect_.store(true);
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
        bool have_command = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait_for(lock, kTickInterval, [this] {
                return !running_.load() || !queue_.empty();
            });
            if (!running_.load()) {
                break;
            }
            if (!queue_.empty()) {
                command = queue_.front();
                queue_.pop_front();
                have_command = true;
            }
        }
        if (have_command) {
            if (command.kind == CommandKind::Shutdown) {
                break;
            }
            handle(command);
        } else {
            tick();
        }
    }
}

void BleWorker::handle(const Command &command)
{
    switch (command.kind) {
    case CommandKind::Connect:
        do_connect();
        break;
    case CommandKind::Scan:
        do_scan();
        break;
    case CommandKind::StopScan:
        do_stop_scan();
        break;
    case CommandKind::GrantPermissions:
        do_grant_permissions();
        break;
    case CommandKind::OpenSettings:
        do_open_settings();
        break;
    case CommandKind::OpenLocationSettings:
        do_open_location_settings();
        break;
    case CommandKind::EnableBluetooth:
        do_enable_bluetooth();
        break;
    case CommandKind::OpenNotificationSettings:
        do_open_notification_settings();
        break;
    case CommandKind::StopPhoneFinder:
        do_stop_phone_finder();
        break;
    case CommandKind::ApplyAutoConnect:
        do_apply_auto_connect();
        break;
    case CommandKind::Refresh:
        do_refresh();
        break;
    case CommandKind::SyncTime:
        do_sync_time();
        break;
    case CommandKind::WriteAlarms:
        if (const auto *write = std::get_if<AlarmWrite>(&command.payload)) {
            do_write_alarms(*write);
        }
        break;
    case CommandKind::WriteTimer:
        if (const auto *timer = std::get_if<wc_program_timer>(
                &command.payload)) {
            do_write_timer(*timer);
        }
        break;
    case CommandKind::WriteCity:
        if (const auto *write = std::get_if<CityWrite>(&command.payload)) {
            do_write_city(*write);
        }
        break;
    case CommandKind::WriteSettings:
        if (const auto *write = std::get_if<SettingsWrite>(&command.payload)) {
            do_write_settings(*write);
        }
        break;
    case CommandKind::RenameWatch:
        if (const auto *write = std::get_if<NameWrite>(&command.payload)) {
            do_rename_watch(*write);
        }
        break;
    case CommandKind::SendNotification:
        if (const auto *write = std::get_if<NotificationWrite>(
                &command.payload)) {
            do_send_notification(*write);
        }
        break;
    case CommandKind::Disconnect:
        do_disconnect();
        break;
    case CommandKind::Shutdown:
        break;
    }
}

bool BleWorker::cancel_requested() const
{
    return !running_.load() || cancel_connect_.load();
}

bool BleWorker::cancel_cb(void *ctx)
{
    return static_cast<BleWorker *>(ctx)->cancel_requested();
}

void BleWorker::set_status(const std::string &status)
{
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.busy = false;
    snapshot_.status = status;
    snapshot_.version++;
}

void BleWorker::tick()
{
    if (!running_.load()) {
        return;
    }
    if (scan_link_ != nullptr) {
        poll_scan();
        return;
    }
    if (session_ != nullptr && link_ != nullptr) {
        // Dispatch Device1 property changes so a drop is noticed without
        // waiting for the next user command.
        (void)link_->pump(0);
        if (link_->link_lost()) {
            handle_link_loss();
            return;
        }
        const bool access = link_->notification_access();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (snapshot_.notification_access != access) {
                snapshot_.notification_access = access;
                snapshot_.version++;
            }
        }
        forward_phone_notification();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (snapshot_.busy) {
                return;
            }
        }
        if (Clock::now() - last_heartbeat_ >= kHeartbeatInterval) {
            heartbeat();
            return;
        }
    }
    if (session_ == nullptr && reconnect_.due(Clock::now())) {
        attempt_connect();
    }
}

void BleWorker::poll_scan()
{
    if (scan_link_ == nullptr) {
        return;
    }
    const std::vector<WatchDevice> devices = scan_link_->devices();
    const bool scanning = scan_link_->scanning();
    const bool permission = scan_link_->permission_missing();
    const bool bluetooth = scan_link_->bluetooth_off();
    const bool location = scan_link_->location_off();
    const std::string error = scan_link_->error();

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        changed = scanning != snapshot_.scanning ||
                  permission != snapshot_.permission_missing ||
                  bluetooth != snapshot_.bluetooth_off ||
                  location != snapshot_.location_off ||
                  !same_devices(devices, snapshot_.devices);
        if (changed) {
            snapshot_.devices = devices;
            snapshot_.scanning = scanning;
            snapshot_.permission_missing = permission;
            snapshot_.bluetooth_off = bluetooth;
            snapshot_.location_off = location;
            if (scanning) {
                snapshot_.status = "scanning for watches...";
            } else if (permission) {
                snapshot_.status = "Bluetooth permission required";
            } else if (bluetooth) {
                snapshot_.status = "Bluetooth is off";
            } else if (location) {
                snapshot_.status = "location services are off";
            } else if (devices.empty()) {
                snapshot_.status = "no watches found";
            } else {
                snapshot_.status = "select a watch to pair";
            }
            snapshot_.error = error;
            snapshot_.busy = false;
            snapshot_.version++;
        }
    }
    if (!scanning && !permission && !bluetooth && !location) {
        reset_scan_link();
    }
}

void BleWorker::reset_scan_link()
{
    if (scan_link_ != nullptr) {
        scan_link_->stop_scan();
        scan_link_.reset();
    }
}

void BleWorker::do_scan()
{
    reconnect_.set_desired(false, Clock::now());
    if (session_ != nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.scanning = false;
        snapshot_.status = "disconnect first";
        snapshot_.version++;
        return;
    }
    reset_scan_link();
    scan_link_ = make_watch_link();
    if (scan_link_ == nullptr || !scan_link_->supports_scan()) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.supports_scan = false;
        snapshot_.scanning = false;
        snapshot_.status = "device list not available on this platform";
        snapshot_.error = "use Connect (scan by watch name)";
        snapshot_.version++;
        reset_scan_link();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.devices.clear();
        snapshot_.scanning = true;
        snapshot_.permission_missing = false;
        snapshot_.bluetooth_off = false;
        snapshot_.location_off = false;
        snapshot_.error.clear();
        snapshot_.status = "scanning for watches...";
        snapshot_.version++;
    }
    if (!scan_link_->start_scan(kScanWindowMs)) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.scanning = false;
        snapshot_.status = "scan could not start";
        snapshot_.error = scan_link_->error();
        snapshot_.version++;
        reset_scan_link();
    }
}

void BleWorker::do_stop_scan()
{
    if (scan_link_ != nullptr) {
        scan_link_->stop_scan();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.scanning = false;
    if (snapshot_.status == "scanning for watches...") {
        snapshot_.status = "scan stopped";
    }
    snapshot_.version++;
}

void BleWorker::do_grant_permissions()
{
    if (scan_link_ == nullptr) {
        scan_link_ = make_watch_link();
    }
    if (scan_link_ == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.status = "waiting for Bluetooth permission...";
        snapshot_.version++;
    }
    scan_link_->request_permissions();
}

void BleWorker::do_open_settings()
{
    std::unique_ptr<WatchLink> temp;
    WatchLink *link = scan_link_.get();
    if (link == nullptr) {
        temp = transient_link();
        link = temp.get();
    }
    if (link != nullptr) {
        link->open_settings();
    }
}

void BleWorker::do_open_location_settings()
{
    std::unique_ptr<WatchLink> temp;
    WatchLink *link = scan_link_.get();
    if (link == nullptr) {
        temp = transient_link();
        link = temp.get();
    }
    if (link != nullptr) {
        link->open_location_settings();
    }
}

void BleWorker::do_enable_bluetooth()
{
    std::unique_ptr<WatchLink> temp;
    WatchLink *link = scan_link_.get();
    if (link == nullptr) {
        temp = transient_link();
        link = temp.get();
    }
    if (link != nullptr) {
        link->enable_bluetooth();
    }
}

void BleWorker::do_open_notification_settings()
{
    std::unique_ptr<WatchLink> temp;
    WatchLink *link = link_ != nullptr ? link_.get() : scan_link_.get();
    if (link == nullptr) {
        temp = transient_link();
        link = temp.get();
    }
    if (link != nullptr) {
        link->open_notification_settings();
    }
}

void BleWorker::do_stop_phone_finder()
{
    std::unique_ptr<WatchLink> temp;
    WatchLink *link = link_ != nullptr ? link_.get() : scan_link_.get();
    if (link == nullptr) {
        temp = transient_link();
        link = temp.get();
    }
    if (link != nullptr) {
        link->stop_phone_finder();
    }
}

void BleWorker::forward_phone_notification()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!snapshot_.connected || snapshot_.busy ||
            !snapshot_.phone_config.forward) {
            return;
        }
    }

    PhoneNotification notification;
    if (!platform_take_notification(notification)) {
        return;
    }
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    const int minute = local.tm_hour * 60 + local.tm_min;

    std::lock_guard<std::mutex> lock(mutex_);
    PhoneConfig config = snapshot_.phone_config;
    if (!phone_notification_allowed(config, notification.app, minute) ||
        notification.app.size() > WC_NOTIFY_FIELD_MAX ||
        notification.title.size() > WC_NOTIFY_FIELD_MAX ||
        notification.text.size() > WC_NOTIFY_FIELD_MAX) {
        snapshot_.notifications_filtered++;
        snapshot_.version++;
        return;
    }
    if (!snapshot_.connected || snapshot_.busy) {
        snapshot_.notifications_filtered++;
        snapshot_.version++;
        return;
    }
    snapshot_.busy = true;
    snapshot_.error.clear();
    snapshot_.status = "forwarding notification from " + notification.app;
    snapshot_.version++;
    queue_.push_back(Command{
        CommandKind::SendNotification,
        NotificationWrite{notification.type, notification.app,
                          notification.title, notification.text, true}});
    condition_.notify_one();
}

void BleWorker::do_apply_auto_connect()
{
    bool on;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        on = start_auto_connect_;
    }
    reconnect_.set_desired(on, Clock::now());
}

std::unique_ptr<WatchLink> BleWorker::transient_link()
{
    return make_watch_link();
}

void BleWorker::handle_link_loss()
{
    if (session_ != nullptr) {
        wc_session_free(session_);
        session_ = nullptr;
    }
    if (link_ != nullptr) {
        link_.reset();
    }

    const bool auto_on = reconnect_.desired();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.connected = false;
        snapshot_.busy = false;
        snapshot_.has_condition = false;
        snapshot_.has_settings = false;
        snapshot_.has_alarms = false;
        snapshot_.has_timer = false;
        snapshot_.has_clock = false;
        snapshot_.has_utc_clock = false;
        memset(&snapshot_.settings, 0, sizeof snapshot_.settings);
        memset(&snapshot_.alarm_first, 0, sizeof snapshot_.alarm_first);
        memset(&snapshot_.alarm_rest, 0, sizeof snapshot_.alarm_rest);
        memset(&snapshot_.timer_value, 0, sizeof snapshot_.timer_value);
        if (auto_on) {
            snapshot_.status = "link lost; reconnecting...";
            append_log(snapshot_, "link lost; reconnecting");
        } else {
            snapshot_.status = "disconnected";
        }
        snapshot_.version++;
    }
    if (auto_on) {
        reconnect_.retry_now(Clock::now());
    }
}

void BleWorker::heartbeat()
{
    if (session_ == nullptr || link_ == nullptr) {
        return;
    }
    last_heartbeat_ = Clock::now();

    wc_basic_setting settings{};
    const int rc = wc_session_read_basic_settings(session_, &settings);
    if (rc == WC_SESSION_OK) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.has_settings = true;
        snapshot_.settings_flags = settings.flags;
        snapshot_.time_24h = settings.time_format_24h != 0;
        snapshot_.version++;
        return;
    }
    if (rc == WC_SESSION_ERR_TIMEOUT || rc == WC_SESSION_ERR_TRANSPORT ||
        link_->link_lost()) {
        handle_link_loss();
    }
}

void BleWorker::schedule_retry(const std::string &why)
{
    if (!running_.load() || !reconnect_.desired()) {
        return;
    }

    // A missing permission or a radio that is off is not a transient scan
    // failure; retrying would just reopen the same dialog or fail forever.
    // Stop the loop and let the pairing page show Grant / Turn on.
    bool blocked = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        blocked = snapshot_.permission_missing || snapshot_.bluetooth_off ||
                  snapshot_.location_off;
        if (blocked) {
            snapshot_.connected = false;
            snapshot_.busy = false;
            snapshot_.status =
                snapshot_.permission_missing
                    ? "Bluetooth permission required"
                    : (snapshot_.bluetooth_off ? "Bluetooth is off"
                                               : "location services are off");
            snapshot_.error.clear();
            snapshot_.version++;
        }
    }
    if (blocked) {
        reconnect_.set_desired(false, Clock::now());
        return;
    }

    reconnect_.on_failure(Clock::now());
    const unsigned failures = reconnect_.failures();
    const auto delay = reconnect_.delay();

    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.connected = false;
    snapshot_.busy = false;
    snapshot_.auto_connect = true;
    snapshot_.demo = false;

    char status[128];
    std::snprintf(status, sizeof status,
                  "watching for the watch (attempt %u, retry in %lld s)",
                  failures, static_cast<long long>(delay.count() / 1000));
    snapshot_.status = status;
    // A plain "not found" is the normal state while the watch is not
    // advertising; keep it out of the red error line and in the log.
    if (why.find("watch not found") != std::string::npos ||
        why.find("cancelled") != std::string::npos) {
        snapshot_.error.clear();
    } else {
        snapshot_.error = why;
    }
    if (failures <= 2 || failures % 5 == 0) {
        append_log(snapshot_, "auto-connect: " + why);
    }
    if (failures == 3) {
        append_log(snapshot_,
                   "still scanning; if the official app is open, turn off "
                   "phone Bluetooth");
    }
    snapshot_.version++;
}

void BleWorker::attempt_connect()
{
    if (!running_.load() || !reconnect_.desired() || session_ != nullptr) {
        return;
    }
    if (cancel_requested()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = true;
        snapshot_.demo = false;
        snapshot_.error.clear();
        snapshot_.status = "scanning for the watch...";
        snapshot_.name.clear();
        snapshot_.model.clear();
        snapshot_.address.clear();
        snapshot_.pairing.clear();
        snapshot_.has_condition = false;
        snapshot_.has_settings = false;
        snapshot_.has_alarms = false;
        snapshot_.has_timer = false;
        snapshot_.has_clock = false;
        snapshot_.has_utc_clock = false;
        memset(&snapshot_.settings, 0, sizeof snapshot_.settings);
        memset(&snapshot_.alarm_first, 0, sizeof snapshot_.alarm_first);
        memset(&snapshot_.alarm_rest, 0, sizeof snapshot_.alarm_rest);
        memset(&snapshot_.timer_value, 0, sizeof snapshot_.timer_value);
        snapshot_.version++;
    }

    TimeConfig config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config = time_config_;
    }
    std::optional<SntpSample> handshake_sample;
    if (config.use_ntp && config.sync_on_connect) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.status = "querying SNTP before connect...";
            snapshot_.version++;
        }
        SntpResult result = query_time_servers(config);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (result.ok) {
                handshake_sample = result.sample;
                snapshot_.has_time_sample = true;
                snapshot_.time_corrected_at = result.sample.corrected_at;
                snapshot_.time_sampled_at = result.sample.sampled_at;
                snapshot_.time_server = result.sample.server;
                snapshot_.time_offset_ms =
                    std::chrono::duration<double, std::milli>(
                        result.sample.offset)
                        .count();
                snapshot_.time_round_trip_ms =
                    std::chrono::duration<double, std::milli>(
                        result.sample.round_trip)
                        .count();
                snapshot_.time_error.clear();
                append_log(snapshot_, "SNTP time acquired from " +
                                          result.sample.server);
            } else {
                snapshot_.time_error =
                    "SNTP unavailable; watch time not changed: " + result.error;
                append_log(snapshot_, snapshot_.time_error);
            }
            snapshot_.version++;
        }
        if (!result.ok) {
            schedule_retry("SNTP unavailable; connection not attempted");
            return;
        }
    }

    std::string target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        target = selected_address_;
        if (!target.empty()) {
            snapshot_.status = "connecting to " + target + "...";
        }
    }

    std::unique_ptr<WatchLink> link = make_watch_link();
    if (link == nullptr) {
        schedule_retry("no Bluetooth transport available");
        return;
    }
    link->set_cancel(&BleWorker::cancel_cb, this);

    int rc = target.empty()
                 ? link->connect(kScanWindowMs)
                 : link->connect_to(target, kScanWindowMs);
    if (rc != WC_SESSION_OK) {
        const bool permission = link->permission_missing();
        const bool bluetooth = link->bluetooth_off();
        const bool location = link->location_off();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.permission_missing = permission;
            snapshot_.bluetooth_off = bluetooth;
            snapshot_.location_off = location;
        }
        const std::string detail = link->error();
        link->close();
        if (cancel_requested()) {
            set_status("cancelled");
            return;
        }
        schedule_retry(detail.empty() ? "watch not found" : detail);
        return;
    }

    uint8_t app_id[WC_APP_ID_LEN];
    std::random_device device;
    for (uint8_t &byte : app_id) {
        byte = static_cast<uint8_t>(device());
    }

    wc_session *session =
        wc_session_new(link->transport(), app_id, 5000);
    if (session == nullptr) {
        link->close();
        schedule_retry("could not allocate the session");
        return;
    }

    wc_identity identity{};
    const wc_current_time time =
        handshake_sample
            ? time_at(handshake_sample->corrected_now(), true)
            : local_time_now();
    rc = wc_session_handshake_with_time(session, &identity, &time, true,
                                        20000);
    if (rc != WC_SESSION_OK) {
        wc_error error{};
        std::string detail = "handshake failed";
        if (wc_session_last_error(session, &error)) {
            char buffer[96];
            std::snprintf(buffer, sizeof buffer,
                          "watch rejected class 0x%02x (status 0x%02x)",
                          error.class_id, error.status);
            detail = buffer;
        } else if (rc == WC_SESSION_ERR_TIMEOUT) {
            detail = "handshake timed out";
        } else if (link->error()[0] != '\0') {
            detail = link->error();
        }
        wc_session_free(session);
        link->close();
        if (cancel_requested()) {
            set_status("cancelled");
            return;
        }
        schedule_retry(detail);
        return;
    }
    if (identity.pairing == WC_PAIRING_BOND_FAILED) {
        wc_session_free(session);
        link->close();
        if (cancel_requested()) {
            set_status("cancelled");
            return;
        }
        schedule_retry("bond failed");
        return;
    }
    if (cancel_requested()) {
        wc_session_free(session);
        link->close();
        set_status("cancelled");
        return;
    }

    link_ = std::move(link);
    session_ = session;
    reconnect_.on_success();
    last_heartbeat_ = Clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.demo = false;
        snapshot_.connected = true;
        snapshot_.busy = false;
        snapshot_.auto_connect = true;
        snapshot_.name = identity.name;
        snapshot_.model =
            identity.model != nullptr ? identity.model->name : "unknown";
        snapshot_.address = link_->address();
        snapshot_.pairing = pairing_name(identity.pairing);
        snapshot_.module_frame = identity.module;
        snapshot_.version_frame = identity.version;
        snapshot_.has_condition = identity.has_condition;
        if (identity.has_condition) {
            snapshot_.battery = identity.condition.battery;
            snapshot_.temperature = identity.condition.temperature;
            snapshot_.mtu = identity.condition.mtu;
        }
        snapshot_.status = "connected: " + snapshot_.name;
        snapshot_.error.clear();
        append_log(snapshot_,
                   "handshake complete; pairing=" + snapshot_.pairing);
        snapshot_.version++;
    }

    // The watch stops advertising after a session, so read everything now
    // while the session is open. Auto-connect keeps it open from here.
    do_refresh();
    if (handshake_sample) {
        do_sync_time(&*handshake_sample);
    }
}

void BleWorker::do_connect()
{
    reset_scan_link();
    cancel_connect_.store(false);
    reconnect_.set_desired(true, Clock::now());

    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.auto_connect = true;
    }
    if (session_ != nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.status = "already connected";
        append_log(snapshot_, "already connected");
        snapshot_.version++;
        return;
    }
    attempt_connect();
}

void BleWorker::do_refresh()
{
    if (session_ == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.status = "not connected";
        append_log(snapshot_, "refresh ignored: not connected");
        snapshot_.version++;
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
    bool two_slot_model = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        two_slot_model = snapshot_.model == "GBX_100";
    }
    ClockRead clock;
    const bool clock_ok = read_clock(session_, clock, !two_slot_model);
    const bool have_clock = !two_slot_model && clock_ok;
    const bool have_utc_clock = clock.has_utc_clock();

    std::string failed;
    if (!have_settings) {
        failed += " 0x13";
    }
    if (!have_alarms) {
        failed += " 0x15/0x16";
    }
    if (!have_timer) {
        failed += " 0x2A";
    }
    if (!have_clock && !have_utc_clock) {
        failed += " 0x1d/0x1e/0x1f";
    }
    last_heartbeat_ = Clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.has_settings = have_settings;
    if (have_settings) {
        snapshot_.settings = settings;
        snapshot_.settings_flags = settings.flags;
        snapshot_.time_24h = settings.time_format_24h != 0;
    }
    snapshot_.has_alarms = have_alarms;
    if (have_alarms) {
        snapshot_.alarm_first = first;
        snapshot_.alarm_rest = rest;
    }
    snapshot_.has_timer = have_timer;
    if (have_timer) {
        snapshot_.timer_value = timer;
    }
    snapshot_.has_clock = have_clock;
    snapshot_.has_utc_clock = have_utc_clock;
    if (have_utc_clock) {
        snapshot_.dst_state = clock.state;
    }
    for (int unit = 0; unit < 5; unit++) {
        if (clock.settings_valid[unit]) {
            snapshot_.dst_settings[unit] = clock.settings[unit];
        }
        if (clock.names_valid[unit]) {
            snapshot_.city_names[unit] = clock.names[unit];
        }
    }
    snapshot_.busy = false;
    snapshot_.error = failed.empty() ? std::string() : "read failed:" + failed;
    snapshot_.status = failed.empty() ? "state refreshed" : "refresh incomplete";
    append_log(snapshot_,
               failed.empty()
                   ? two_slot_model
                         ? "read settings, alarms, timer and Home/World"
                         : "read settings, alarms, timer and five cities"
                   : "refresh incomplete:" + failed);
    snapshot_.version++;
}

void BleWorker::do_sync_time(const SntpSample *prefetched)
{
    TimeConfig config;
    bool demo = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config = time_config_;
        demo = snapshot_.demo;
        snapshot_.busy = true;
    }
    if (demo) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.error.clear();
        snapshot_.status = config.use_ntp
                               ? "demo: SNTP disabled; no watch write"
                               : "demo: local time sync simulated";
        if (config.use_ntp) {
            snapshot_.time_error =
                "the recorded demo never accesses SNTP servers";
        }
        append_log(snapshot_, snapshot_.status);
        snapshot_.version++;
        return;
    }
    if (session_ == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.status = "not connected";
        append_log(snapshot_, "sync ignored: not connected");
        snapshot_.version++;
        return;
    }

    auto fail = [this](const std::string &status, const std::string &error) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.error = error;
        snapshot_.status = status;
        append_log(snapshot_, status + ": " + error);
        snapshot_.version++;
    };

    SntpSample sample;
    if (config.use_ntp) {
        if (prefetched != nullptr) {
            sample = *prefetched;
        } else {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.status = "querying SNTP servers...";
                snapshot_.version++;
            }
            SntpResult result = query_time_servers(config);
            if (!result.ok) {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.busy = false;
                snapshot_.time_error = result.error;
                snapshot_.error = "NTP sync failed; watch not changed";
                snapshot_.status = snapshot_.error;
                append_log(snapshot_, "SNTP failed: " + result.error);
                snapshot_.version++;
                return;
            }
            sample = result.sample;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.has_time_sample = true;
        snapshot_.time_corrected_at = sample.corrected_at;
        snapshot_.time_sampled_at = sample.sampled_at;
        snapshot_.time_server = sample.server;
        snapshot_.time_offset_ms =
            std::chrono::duration<double, std::milli>(sample.offset).count();
        snapshot_.time_round_trip_ms =
            std::chrono::duration<double, std::milli>(sample.round_trip).count();
        snapshot_.time_error.clear();
        snapshot_.version++;
    }

    UtcClock clock;
    if (config.use_ntp) {
        bool have_clock = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            have_clock = snapshot_.has_utc_clock;
            if (have_clock) {
                clock.state = snapshot_.dst_state;
                memcpy(clock.settings, snapshot_.dst_settings,
                       sizeof clock.settings);
                memcpy(clock.names, snapshot_.city_names, sizeof clock.names);
            }
        }
        if (!have_clock) {
            std::string read_error;
            if (!read_utc_clock(session_, clock, &read_error)) {
                fail("UTC setup read failed; watch not changed", read_error);
                return;
            }
        }
        configure_utc_clock(clock);

        int rc = wc_session_write_dst_watch_state(session_, &clock.state);
        if (rc != WC_SESSION_OK) {
            fail("UTC write failed at 0x1d; time not written",
                 "Home/World DST state write failed");
            return;
        }
        for (uint8_t unit = 0; unit < 2; unit++) {
            rc = wc_session_write_dst_setting(session_, &clock.settings[unit]);
            if (rc != WC_SESSION_OK) {
                fail("0x1d written; UTC setup incomplete",
                     std::string(unit == 0 ? "Home" : "World") +
                         " timezone write 0x1e failed");
                return;
            }
            rc = wc_session_write_city_name(session_, &clock.names[unit]);
            if (rc != WC_SESSION_OK) {
                fail("0x1d/0x1e written; UTC setup incomplete",
                     std::string(unit == 0 ? "Home" : "World") +
                         " city name write 0x1f failed");
                return;
            }
        }
    }

    const wc_current_time value =
        config.use_ntp ? time_at(sample.corrected_now(), true)
                       : local_time_now();
    uint8_t frame[WC_LEN_CURRENT_TIME];
    size_t length = 0;
    if (wc_current_time_encode(&value, frame, sizeof frame, &length) != 0) {
        fail(config.use_ntp ? "Home/World UTC written; time encode failed"
                            : "time write failed",
             "could not encode the time frame");
        return;
    }
    const int rc = wc_session_write_all(session_, frame, length);
    if (rc != WC_SESSION_OK) {
        const bool lost = link_->link_lost() ||
                          rc == WC_SESSION_ERR_TIMEOUT ||
                          rc == WC_SESSION_ERR_TRANSPORT;
        fail(config.use_ntp ? "Home/World UTC written; time write failed"
                            : "time write failed",
             "time write 0x09 failed");
        if (lost) {
            handle_link_loss();
        }
        return;
    }
    last_heartbeat_ = Clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.busy = false;
    if (config.use_ntp) {
        snapshot_.dst_state = clock.state;
        snapshot_.has_utc_clock = true;
        memcpy(snapshot_.dst_settings, clock.settings, sizeof clock.settings);
        memcpy(snapshot_.city_names, clock.names, sizeof clock.names);
        snapshot_.error.clear();
        snapshot_.status = "Home/World UTC and SNTP time written";
    } else {
        snapshot_.time_error.clear();
        snapshot_.error.clear();
        snapshot_.status = "device local time written; check the watch";
    }
    append_log(snapshot_, snapshot_.status +
                              (config.use_ntp ? " from " + sample.server
                                              : std::string()));
    snapshot_.version++;
}

void BleWorker::do_write_alarms(const AlarmWrite &write)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.demo) {
            snapshot_.alarm_first = write.first;
            snapshot_.alarm_rest = write.rest;
            snapshot_.has_alarms = true;
            snapshot_.busy = false;
            snapshot_.error.clear();
            snapshot_.status = "alarms updated in the demo session";
            append_log(snapshot_, "demo: alarms written to the session");
            snapshot_.version++;
            return;
        }
    }
    if (session_ == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.status = "not connected";
        snapshot_.version++;
        return;
    }

    const int rc = wc_session_write_alarms(session_, &write.first, &write.rest);
    if (rc != WC_SESSION_OK) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.error = "alarm write failed";
        snapshot_.status = "alarm write failed";
        append_log(snapshot_, "alarm write failed");
        snapshot_.version++;
        return;
    }

    wc_alarm_first first{};
    wc_alarm_rest rest{};
    const bool readback = wc_session_read_alarms(session_, &first, &rest) ==
                          WC_SESSION_OK;
    last_heartbeat_ = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.busy = false;
    snapshot_.error.clear();
    if (readback) {
        snapshot_.alarm_first = first;
        snapshot_.alarm_rest = rest;
        snapshot_.status = "alarms written and read back";
        append_log(snapshot_, "alarms written and read back");
    } else {
        snapshot_.alarm_first = write.first;
        snapshot_.alarm_rest = write.rest;
        snapshot_.status = "alarms written; readback failed";
        append_log(snapshot_, "alarms written; readback failed");
    }
    snapshot_.has_alarms = true;
    snapshot_.version++;
}

void BleWorker::do_write_timer(const wc_program_timer &timer)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.demo) {
            snapshot_.timer_value = timer;
            snapshot_.has_timer = true;
            snapshot_.busy = false;
            snapshot_.error.clear();
            snapshot_.status = "program timer updated in the demo session";
            append_log(snapshot_, "demo: program timer replaced");
            snapshot_.version++;
            return;
        }
    }
    if (session_ == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.status = "not connected";
        snapshot_.version++;
        return;
    }

    const int rc = wc_session_write_program_timer(session_, &timer);
    if (rc != WC_SESSION_OK) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.error = "program timer write failed";
        snapshot_.status = "program timer write failed";
        append_log(snapshot_, "program timer write failed");
        snapshot_.version++;
        return;
    }

    wc_program_timer readback{};
    const bool verified = wc_session_read_program_timer(session_, &readback) ==
                          WC_SESSION_OK;
    last_heartbeat_ = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.busy = false;
    snapshot_.error.clear();
    snapshot_.timer_value = verified ? readback : timer;
    snapshot_.has_timer = true;
    snapshot_.status = verified ? "program timer written and read back"
                                : "program timer written; readback failed";
    append_log(snapshot_, snapshot_.status);
    snapshot_.version++;
}

void BleWorker::do_write_city(const CityWrite &write)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.demo) {
            snapshot_.dst_state = write.state;
            snapshot_.dst_settings[write.unit] = write.setting;
            snapshot_.city_names[write.unit] = write.name;
            snapshot_.has_clock = true;
            snapshot_.busy = false;
            snapshot_.error.clear();
            snapshot_.status = "city and DST updated in the demo session";
            append_log(snapshot_, "demo: city and DST settings written");
            snapshot_.version++;
            return;
        }
    }
    if (session_ == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.status = "not connected";
        snapshot_.version++;
        return;
    }

    auto fail = [this](const std::string &status, const std::string &error) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.error = error;
        snapshot_.status = status;
        append_log(snapshot_, status + ": " + error);
        snapshot_.version++;
    };
    if (write.unit < 2 &&
        wc_session_write_dst_watch_state(session_, &write.state) !=
            WC_SESSION_OK) {
        fail("city write failed at 0x1d", "DST watch state write failed");
        return;
    }
    if (wc_session_write_dst_setting(session_, &write.setting) != WC_SESSION_OK) {
        fail(write.unit < 2 ? "0x1d written; city setup incomplete"
                            : "city write failed at 0x1e",
             "timezone/DST setting write failed");
        return;
    }
    if (wc_session_write_city_name(session_, &write.name) != WC_SESSION_OK) {
        fail("city setting written; city name write failed",
             "city label write 0x1f failed");
        return;
    }

    ClockRead read;
    const bool readback = read_clock(session_, read, true);
    last_heartbeat_ = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.busy = false;
    snapshot_.error.clear();
    snapshot_.has_clock = true;
    snapshot_.has_utc_clock =
        snapshot_.has_utc_clock || read.has_utc_clock() || write.unit < 2;
    if (read.has_utc_clock()) {
        snapshot_.dst_state = read.state;
    }
    for (int unit = 0; unit < 5; unit++) {
        if (read.settings_valid[unit]) {
            snapshot_.dst_settings[unit] = read.settings[unit];
        }
        if (read.names_valid[unit]) {
            snapshot_.city_names[unit] = read.names[unit];
        }
    }
    if (readback) {
        snapshot_.status = "city and DST written and read back";
    } else {
        snapshot_.dst_state = write.state;
        snapshot_.dst_settings[write.unit] = write.setting;
        snapshot_.city_names[write.unit] = write.name;
        snapshot_.status = "city and DST written; full readback failed";
    }
    append_log(snapshot_, snapshot_.status);
    snapshot_.version++;
}

void BleWorker::do_write_settings(const SettingsWrite &write)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.demo) {
            snapshot_.settings = write.settings;
            snapshot_.settings_flags = write.settings.flags;
            snapshot_.time_24h = write.settings.time_format_24h != 0;
            snapshot_.has_settings = true;
            if (write.has_name) {
                snapshot_.name = write.name;
            }
            snapshot_.busy = false;
            snapshot_.error.clear();
            snapshot_.status = "settings updated in the demo session";
            append_log(snapshot_, "demo: basic settings written");
            snapshot_.version++;
            return;
        }
    }
    if (session_ == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.status = "not connected";
        snapshot_.version++;
        return;
    }

    const int rc = wc_session_write_basic_settings(session_, &write.settings);
    if (rc != WC_SESSION_OK) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.error = "settings write failed";
        snapshot_.status = "settings write failed";
        append_log(snapshot_, "settings write failed");
        snapshot_.version++;
        return;
    }

    bool name_written = false;
    bool name_failed = false;
    if (write.has_name) {
        const int name_rc = wc_session_write_watch_name(session_, write.name);
        name_written = name_rc == WC_SESSION_OK;
        name_failed = !name_written;
    }

    wc_basic_setting readback{};
    const bool verified = wc_session_read_basic_settings(session_, &readback) ==
                          WC_SESSION_OK;
    last_heartbeat_ = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.busy = false;
    snapshot_.error.clear();
    snapshot_.settings = verified ? readback : write.settings;
    snapshot_.settings_flags = snapshot_.settings.flags;
    snapshot_.time_24h = snapshot_.settings.time_format_24h != 0;
    snapshot_.has_settings = true;
    if (name_written) {
        snapshot_.name = write.name;
    }
    if (name_failed) {
        snapshot_.error = "watch name write failed";
        snapshot_.status = "settings written; name write failed";
    } else if (verified) {
        snapshot_.status = write.has_name ? "settings and name written"
                                         : "settings written and read back";
    } else {
        snapshot_.status = "settings written; readback failed";
    }
    append_log(snapshot_, snapshot_.status);
    snapshot_.version++;
}

void BleWorker::do_rename_watch(const NameWrite &write)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.demo) {
            snapshot_.name = write.name;
            snapshot_.busy = false;
            snapshot_.error.clear();
            snapshot_.status = "watch name updated in the demo session";
            append_log(snapshot_, "demo: watch name written");
            snapshot_.version++;
            return;
        }
    }
    if (session_ == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.status = "not connected";
        snapshot_.version++;
        return;
    }

    const int rc = wc_session_write_watch_name(session_, write.name);
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.busy = false;
    if (rc == WC_SESSION_OK) {
        snapshot_.name = write.name;
        snapshot_.error.clear();
        snapshot_.status = "watch name written";
        append_log(snapshot_, "watch name written");
    } else {
        snapshot_.error = "watch name write failed";
        snapshot_.status = "watch name write failed";
        append_log(snapshot_, "watch name write failed");
    }
    snapshot_.version++;
}

void BleWorker::do_send_notification(const NotificationWrite &write)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.demo) {
            snapshot_.busy = false;
            snapshot_.error.clear();
            snapshot_.status = "notification sent in the demo session";
            append_log(snapshot_, "demo: notification encoded and dropped");
            snapshot_.version++;
            return;
        }
    }
    if (session_ == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.status = "not connected";
        snapshot_.version++;
        return;
    }

    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    wc_notification notification;
    memset(&notification, 0, sizeof notification);
    notification.type = write.type;
    std::strftime(notification.timestamp, sizeof notification.timestamp,
                  "%Y%m%dT%H%M%S", &local);
    std::snprintf(notification.app, sizeof notification.app, "%s",
                  write.app.c_str());
    std::snprintf(notification.title, sizeof notification.title, "%s",
                  write.title.c_str());
    std::snprintf(notification.text, sizeof notification.text, "%s",
                  write.text.c_str());

    uint8_t packet[WC_NOTIFY_MAX];
    size_t length = 0;
    int rc = wc_notification_encode(&notification, packet, sizeof packet,
                                    &length);
    if (rc != WC_NOTIFY_OK) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.busy = false;
        snapshot_.error = "notification fields are too long";
        snapshot_.status = "notification not sent";
        append_log(snapshot_, "notification encode failed");
        snapshot_.version++;
        return;
    }
    rc = wc_session_send_notification(session_, packet, length);
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.busy = false;
    if (rc == WC_SESSION_OK) {
        snapshot_.error.clear();
        snapshot_.status = write.automatic ? "phone notification forwarded"
                                           : "notification sent";
        if (write.automatic) {
            snapshot_.notifications_forwarded++;
        }
        append_log(snapshot_, snapshot_.status);
    } else {
        snapshot_.error = "notification send failed";
        snapshot_.status = "notification send failed";
        append_log(snapshot_, "notification send failed");
    }
    snapshot_.version++;
}

void BleWorker::do_disconnect()
{
    reconnect_.set_desired(false, Clock::now());
    cancel_connect_.store(false);
    reset_scan_link();

    if (session_ != nullptr) {
        wc_session_free(session_);
        session_ = nullptr;
    }
    if (link_ != nullptr) {
        link_.reset();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.connected = false;
    snapshot_.busy = false;
    snapshot_.auto_connect = false;
    snapshot_.scanning = false;
    snapshot_.selected_address.clear();
    selected_address_.clear();
    snapshot_.status = "disconnected";
    snapshot_.error.clear();
    append_log(snapshot_, "disconnected; auto-connect off");
    snapshot_.version++;
}

void BleWorker::cleanup()
{
    reset_scan_link();
    if (session_ != nullptr) {
        wc_session_free(session_);
        session_ = nullptr;
    }
    if (link_ != nullptr) {
        link_.reset();
    }
}

} // namespace wristctl::gui
