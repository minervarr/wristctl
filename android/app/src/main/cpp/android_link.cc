#include "watch_link.hh"

#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "android_link.hh"

#define LOG_TAG "WristctlJni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace wristctl::gui {
namespace {

JavaVM *g_vm = nullptr;
jobject g_activity = nullptr;  // global ref
std::mutex g_global_mu;
std::mutex g_notification_mu;
std::deque<PhoneNotification> g_notifications;

class AndroidWatchLink;

std::mutex g_registry_mu;
std::unordered_map<jlong, AndroidWatchLink *> g_registry;
std::atomic<jlong> g_next_handle{1};

JNIEnv *env_for_worker()
{
    if (g_vm == nullptr) {
        return nullptr;
    }
    JNIEnv *env = nullptr;
    const jint rc = g_vm->GetEnv(reinterpret_cast<void **>(&env),
                                 JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("AttachCurrentThread failed");
            return nullptr;
        }
    } else if (rc != JNI_OK) {
        LOGE("GetEnv failed: %d", rc);
        return nullptr;
    }
    return env;
}

bool check_exc(JNIEnv *env, const char *where)
{
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        LOGE("JNI exception at %s", where);
        return true;
    }
    return false;
}

std::string to_string(JNIEnv *env, jstring value)
{
    if (value == nullptr) {
        return {};
    }
    const char *chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        check_exc(env, "GetStringUTFChars");
        return {};
    }
    std::string out(chars);
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

template <typename Fn>
void dispatch(jlong handle, Fn &&fn)
{
    std::lock_guard<std::mutex> lock(g_registry_mu);
    auto it = g_registry.find(handle);
    if (it != g_registry.end()) {
        fn(*it->second);
    }
}

class AndroidWatchLink final : public WatchLink {
public:
    AndroidWatchLink()
        : handle_(g_next_handle.fetch_add(1))
    {
        transport_.ctx = this;
        transport_.write = &AndroidWatchLink::write_thunk;
        transport_.read = &AndroidWatchLink::read_thunk;
        transport_.write_link_loss_alert = &AndroidWatchLink::lla_thunk;
        transport_.pair = &AndroidWatchLink::pair_thunk;
        transport_.close = &AndroidWatchLink::close_thunk;
        transport_.now_ms = &AndroidWatchLink::now_thunk;
        if (!ensure_java()) {
            return;
        }
        std::lock_guard<std::mutex> lock(g_registry_mu);
        g_registry[handle_] = this;
    }

    ~AndroidWatchLink() override { close(); }

    // ── WatchLink ───────────────────────────────────────────────────────────
    void set_cancel(WatchLinkCancel cancel, void *ctx) override
    {
        cancel_ = cancel;
        cancel_ctx_ = ctx;
    }

    int connect(uint32_t scan_timeout_ms) override
    {
        return connect_common(-1, scan_timeout_ms);
    }

    int connect_to(const std::string &address, uint32_t timeout_ms) override
    {
        if (address.empty()) {
            return WC_SESSION_ERR_INVALID;
        }
        target_address_ = address;
        return connect_common(1, timeout_ms);
    }

    const wc_transport *transport() const override { return &transport_; }

    int pump(uint32_t) override { return 0; }

    bool link_lost() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return link_lost_;
    }

    const char *address() const override { return address_.c_str(); }

    const char *error() const override { return error_.c_str(); }

    void close() override;

    bool supports_scan() const override { return true; }

    bool start_scan(uint32_t scan_timeout_ms) override
    {
        if (!java_ready_) {
            return false;
        }
        JNIEnv *env = env_for_worker();
        if (env == nullptr) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            devices_.clear();
            scanning_ = true;
            permission_missing_ = false;
            bluetooth_off_ = false;
            location_off_ = false;
            error_.clear();
        }
        const jboolean ok = env->CallBooleanMethod(
            ble_, m_start_scan_, handle_,
            static_cast<jint>(scan_timeout_ms));
        if (check_exc(env, "BleLink.startScan") || ok != JNI_TRUE) {
            std::lock_guard<std::mutex> lock(mu_);
            scanning_ = false;
            return false;
        }
        return true;
    }

    void stop_scan() override
    {
        if (!java_ready_) {
            return;
        }
        JNIEnv *env = env_for_worker();
        if (env == nullptr) {
            return;
        }
        env->CallVoidMethod(ble_, m_stop_scan_, handle_);
        check_exc(env, "BleLink.stopScan");
        std::lock_guard<std::mutex> lock(mu_);
        scanning_ = false;
    }

    bool scanning() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return scanning_;
    }

    std::vector<WatchDevice> devices() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return devices_;
    }

    bool permission_missing() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return permission_missing_;
    }

    bool bluetooth_off() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return bluetooth_off_;
    }

    bool location_off() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return location_off_;
    }

    bool pairing() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return pairing_;
    }

    void request_permissions() override
    {
        if (!java_ready_) {
            return;
        }
        JNIEnv *env = env_for_worker();
        if (env == nullptr) {
            return;
        }
        env->CallVoidMethod(ble_, m_request_permissions_, handle_);
        check_exc(env, "BleLink.requestPermissions");
    }

    void open_settings() override
    {
        if (!java_ready_) {
            return;
        }
        JNIEnv *env = env_for_worker();
        if (env == nullptr) {
            return;
        }
        env->CallVoidMethod(ble_, m_open_settings_, handle_);
        check_exc(env, "BleLink.openAppSettings");
    }

    void open_location_settings() override
    {
        if (!java_ready_) {
            return;
        }
        JNIEnv *env = env_for_worker();
        if (env == nullptr) {
            return;
        }
        env->CallVoidMethod(ble_, m_open_location_settings_, handle_);
        check_exc(env, "BleLink.openLocationSettings");
    }

    void enable_bluetooth() override
    {
        if (!java_ready_) {
            return;
        }
        JNIEnv *env = env_for_worker();
        if (env == nullptr) {
            return;
        }
        env->CallVoidMethod(ble_, m_enable_bluetooth_, handle_);
        check_exc(env, "BleLink.enableBluetooth");
    }

    void open_notification_settings() override
    {
        if (!java_ready_) {
            return;
        }
        JNIEnv *env = env_for_worker();
        if (env == nullptr) {
            return;
        }
        env->CallVoidMethod(ble_, m_open_notification_settings_, handle_);
        check_exc(env, "BleLink.openNotificationSettings");
    }

    void stop_phone_finder() override
    {
        if (!java_ready_) {
            return;
        }
        JNIEnv *env = env_for_worker();
        if (env == nullptr) {
            return;
        }
        env->CallVoidMethod(ble_, m_stop_phone_finder_, handle_);
        check_exc(env, "BleLink.stopPhoneFinder");
    }

    bool notification_access() const override
    {
        if (!java_ready_) {
            return false;
        }
        JNIEnv *env = env_for_worker();
        if (env == nullptr) {
            return false;
        }
        const jboolean granted = env->CallBooleanMethod(
            ble_, m_notification_access_, handle_);
        return !check_exc(env, "BleLink.notificationAccess") &&
               granted == JNI_TRUE;
    }

    // ── Callbacks from Java (called with the registry lock held) ────────────
    void on_connected(JNIEnv *env, jstring address, jstring name)
    {
        std::lock_guard<std::mutex> lock(mu_);
        address_ = to_string(env, address);
        name_ = to_string(env, name);
        connected_ = true;
        connect_done_ = true;
        connect_ok_ = true;
        link_lost_ = false;
        error_.clear();
        cv_.notify_all();
    }

    void on_disconnected(int status)
    {
        std::lock_guard<std::mutex> lock(mu_);
        connected_ = false;
        link_lost_ = true;
        connect_done_ = true;
        connect_ok_ = false;
        if (pairing_) {
            bond_done_ = true;
            bond_ok_ = 0;
            pairing_ = false;
        }
        error_ = "link lost (status " + std::to_string(status) + ")";
        cv_.notify_all();
    }

    void on_frame(JNIEnv *env, jbyteArray value)
    {
        if (value == nullptr) {
            return;
        }
        const jsize length = env->GetArrayLength(value);
        if (length <= 0 || length > 512) {
            return;
        }
        std::vector<uint8_t> frame(static_cast<size_t>(length));
        env->GetByteArrayRegion(value, 0, length,
                                reinterpret_cast<jbyte *>(frame.data()));
        if (check_exc(env, "GetByteArrayRegion")) {
            return;
        }
        std::lock_guard<std::mutex> lock(mu_);
        frames_.push_back(std::move(frame));
        if (frames_.size() > 64) {
            frames_.pop_front();
        }
        cv_.notify_all();
    }

    void on_write(int seq, int status)
    {
        std::lock_guard<std::mutex> lock(mu_);
        write_done_ = true;
        write_seq_ = seq;
        write_status_ = status;
        cv_.notify_all();
    }

    void on_bond(int bonded)
    {
        std::lock_guard<std::mutex> lock(mu_);
        bond_done_ = true;
        bond_ok_ = bonded != 0 ? 1 : 0;
        pairing_ = false;
        cv_.notify_all();
    }

    void on_error(JNIEnv *env, jstring message)
    {
        std::lock_guard<std::mutex> lock(mu_);
        error_ = to_string(env, message);
        scanning_ = false;
        connect_done_ = true;
        connect_ok_ = false;
        if (pairing_) {
            bond_done_ = true;
            bond_ok_ = 0;
            pairing_ = false;
        }
        cv_.notify_all();
    }

    void on_scan_state(int state)
    {
        std::lock_guard<std::mutex> lock(mu_);
        switch (state) {
        case 0:
            scanning_ = false;
            break;
        case 1:
            scanning_ = true;
            permission_missing_ = false;
            bluetooth_off_ = false;
            location_off_ = false;
            break;
        case 2:
            scanning_ = false;
            permission_missing_ = true;
            if (!connected_) {
                connect_done_ = true;
                connect_ok_ = false;
                error_ = "Bluetooth permission required";
            }
            break;
        case 3:
            scanning_ = false;
            bluetooth_off_ = true;
            if (!connected_) {
                connect_done_ = true;
                connect_ok_ = false;
                error_ = "Bluetooth is off";
            }
            break;
        case 4:
            scanning_ = false;
            location_off_ = true;
            if (!connected_) {
                connect_done_ = true;
                connect_ok_ = false;
                error_ = "location services are off";
            }
            break;
        default:
            break;
        }
        cv_.notify_all();
    }

    void on_device(JNIEnv *env, jstring name, jstring address, int rssi,
                   bool bonded)
    {
        WatchDevice device;
        device.name = to_string(env, name);
        device.address = to_string(env, address);
        device.rssi = rssi;
        device.bonded = bonded;
        if (device.address.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mu_);
        for (WatchDevice &existing : devices_) {
            if (existing.address == device.address) {
                existing = device;
                return;
            }
        }
        if (devices_.size() < 64) {
            devices_.push_back(std::move(device));
        }
    }

    const char *name() const { return name_.c_str(); }

private:
    int connect_common(int mode, uint32_t timeout_ms)
    {
        if (!java_ready_) {
            return WC_SESSION_ERR_TRANSPORT;
        }
        JNIEnv *env = env_for_worker();
        if (env == nullptr) {
            return WC_SESSION_ERR_TRANSPORT;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            connect_done_ = false;
            connect_ok_ = false;
            link_lost_ = false;
            error_.clear();
            frames_.clear();
            scanning_ = false;
        }

        jboolean started = JNI_FALSE;
        if (mode == 1) {
            jstring target = env->NewStringUTF(target_address_.c_str());
            started = env->CallBooleanMethod(
                ble_, m_connect_to_, handle_, target,
                static_cast<jint>(timeout_ms),
                static_cast<jint>(connect_timeout_ms_));
            env->DeleteLocalRef(target);
        } else {
            started = env->CallBooleanMethod(
                ble_, m_connect_, handle_,
                static_cast<jint>(timeout_ms),
                static_cast<jint>(connect_timeout_ms_));
        }
        if (check_exc(env, "BleLink.connect")) {
            error_ = "JNI connect call failed";
            return WC_SESSION_ERR_TRANSPORT;
        }
        if (started != JNI_TRUE) {
            error_ = "BLE link is closed";
            return WC_SESSION_ERR_TRANSPORT;
        }

        std::unique_lock<std::mutex> lock(mu_);
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeout_ms + 30000);
        for (;;) {
            if (cancel_ != nullptr && cancel_(cancel_ctx_)) {
                lock.unlock();
                env->CallVoidMethod(ble_, m_disconnect_, handle_);
                check_exc(env, "BleLink.disconnect(cancel)");
                lock.lock();
                error_ = "cancelled";
                return WC_SESSION_ERR_TRANSPORT;
            }
            if (connect_done_) {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                lock.unlock();
                env->CallVoidMethod(ble_, m_disconnect_, handle_);
                check_exc(env, "BleLink.disconnect(timeout)");
                lock.lock();
                error_ = "connect timed out";
                return WC_SESSION_ERR_TIMEOUT;
            }
            cv_.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (!connect_ok_) {
            if (error_.empty()) {
                error_ = "connection failed";
            }
            return WC_SESSION_ERR_TRANSPORT;
        }
        transport_.address = address_.c_str();
        return WC_SESSION_OK;
    }

    bool ensure_java()
    {
        if (java_ready_) {
            return true;
        }
        JNIEnv *env = env_for_worker();
        if (env == nullptr || g_activity == nullptr) {
            error_ = "no Android activity for the BLE bridge";
            return false;
        }

        jclass activity_class = env->GetObjectClass(g_activity);
        jmethodID get_class_loader = env->GetMethodID(
            activity_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
        if (check_exc(env, "getClassLoader") || get_class_loader == nullptr) {
            env->DeleteLocalRef(activity_class);
            error_ = "cannot find the app classloader";
            return false;
        }
        jobject loader = env->CallObjectMethod(g_activity, get_class_loader);
        env->DeleteLocalRef(activity_class);
        if (check_exc(env, "CallObjectMethod(getClassLoader)") || loader == nullptr) {
            error_ = "cannot get the app classloader";
            return false;
        }
        jclass loader_class = env->GetObjectClass(loader);
        jmethodID load_class = env->GetMethodID(
            loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        if (check_exc(env, "loadClass method") || load_class == nullptr) {
            env->DeleteLocalRef(loader_class);
            env->DeleteLocalRef(loader);
            error_ = "cannot load BleLink class";
            return false;
        }
        jstring class_name = env->NewStringUTF("dev.wristctl.BleLink");
        jclass local_class = static_cast<jclass>(
            env->CallObjectMethod(loader, load_class, class_name));
        env->DeleteLocalRef(class_name);
        env->DeleteLocalRef(loader_class);
        env->DeleteLocalRef(loader);
        if (check_exc(env, "loadClass(BleLink)") || local_class == nullptr) {
            error_ = "BleLink class not found";
            return false;
        }

        jmethodID ctor = env->GetMethodID(
            local_class, "<init>", "(Landroid/app/Activity;)V");
        if (check_exc(env, "BleLink ctor") || ctor == nullptr) {
            env->DeleteLocalRef(local_class);
            error_ = "BleLink constructor not found";
            return false;
        }
        jobject local = env->NewObject(local_class, ctor, g_activity);
        if (check_exc(env, "new BleLink") || local == nullptr) {
            env->DeleteLocalRef(local_class);
            error_ = "cannot construct BleLink";
            return false;
        }

        ble_class_ = static_cast<jclass>(env->NewGlobalRef(local_class));
        ble_ = env->NewGlobalRef(local);
        env->DeleteLocalRef(local);
        env->DeleteLocalRef(local_class);
        if (ble_ == nullptr || ble_class_ == nullptr) {
            error_ = "cannot keep BleLink";
            return false;
        }

        m_set_handle_ = env->GetMethodID(ble_class_, "setHandle", "(J)V");
        m_connect_ = env->GetMethodID(ble_class_, "connect", "(JII)Z");
        m_connect_to_ = env->GetMethodID(
            ble_class_, "connectTo", "(JLjava/lang/String;II)Z");
        m_start_scan_ = env->GetMethodID(ble_class_, "startScan", "(JI)Z");
        m_stop_scan_ = env->GetMethodID(ble_class_, "stopScan", "(J)V");
        m_write_ = env->GetMethodID(ble_class_, "write", "(JI[B)I");
        m_pair_ = env->GetMethodID(ble_class_, "pair", "(J)Z");
        m_lla_ = env->GetMethodID(ble_class_, "writeLinkLossAlert", "(JI)Z");
        m_is_bonded_ = env->GetMethodID(ble_class_, "isBonded", "(J)Z");
        m_disconnect_ = env->GetMethodID(ble_class_, "disconnect", "(J)V");
        m_request_permissions_ = env->GetMethodID(
            ble_class_, "requestPermissions", "(J)V");
        m_open_settings_ = env->GetMethodID(
            ble_class_, "openAppSettings", "(J)V");
        m_open_location_settings_ = env->GetMethodID(
            ble_class_, "openLocationSettings", "(J)V");
        m_enable_bluetooth_ = env->GetMethodID(
            ble_class_, "enableBluetooth", "(J)V");
        m_open_notification_settings_ = env->GetMethodID(
            ble_class_, "openNotificationSettings", "(J)V");
        m_stop_phone_finder_ = env->GetMethodID(
            ble_class_, "stopPhoneFinder", "(J)V");
        m_notification_access_ = env->GetMethodID(
            ble_class_, "notificationAccess", "(J)Z");
        if (check_exc(env, "BleLink method lookup") || m_set_handle_ == nullptr ||
            m_connect_ == nullptr || m_connect_to_ == nullptr ||
            m_start_scan_ == nullptr || m_stop_scan_ == nullptr ||
            m_write_ == nullptr || m_pair_ == nullptr || m_lla_ == nullptr ||
            m_is_bonded_ == nullptr || m_disconnect_ == nullptr ||
            m_request_permissions_ == nullptr || m_open_settings_ == nullptr ||
            m_open_location_settings_ == nullptr ||
            m_enable_bluetooth_ == nullptr ||
            m_open_notification_settings_ == nullptr ||
            m_stop_phone_finder_ == nullptr || m_notification_access_ == nullptr) {
            error_ = "BleLink methods missing";
            return false;
        }
        env->CallVoidMethod(ble_, m_set_handle_, handle_);
        if (check_exc(env, "BleLink.setHandle")) {
            error_ = "BleLink.setHandle failed";
            return false;
        }
        java_ready_ = true;
        LOGI("BleLink loaded (handle %lld)", static_cast<long long>(handle_));
        return true;
    }

    // ── wc_transport thunks ─────────────────────────────────────────────────
    static int write_thunk(void *ctx, wc_write_kind kind, const uint8_t *data,
                           size_t len)
    {
        return static_cast<AndroidWatchLink *>(ctx)->write(kind, data, len);
    }

    static int read_thunk(void *ctx, uint8_t *buf, size_t cap,
                          uint32_t timeout_ms)
    {
        return static_cast<AndroidWatchLink *>(ctx)->read(buf, cap, timeout_ms);
    }

    static int lla_thunk(void *ctx, uint8_t level)
    {
        return static_cast<AndroidWatchLink *>(ctx)->write_link_loss_alert(
            level);
    }

    static int pair_thunk(void *ctx, const char *address)
    {
        (void)address;
        return static_cast<AndroidWatchLink *>(ctx)->pair();
    }

    static void close_thunk(void *ctx)
    {
        static_cast<AndroidWatchLink *>(ctx)->close();
    }

    static uint64_t now_thunk(void *)
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    int write(wc_write_kind kind, const uint8_t *data, size_t len)
    {
        if (!java_ready_ || data == nullptr || len == 0) {
            return -1;
        }
        JNIEnv *env = env_for_worker();
        if (env == nullptr) {
            return -1;
        }
        jbyteArray array = env->NewByteArray(static_cast<jsize>(len));
        if (array == nullptr) {
            check_exc(env, "NewByteArray");
            return -1;
        }
        env->SetByteArrayRegion(array, 0, static_cast<jsize>(len),
                                reinterpret_cast<const jbyte *>(data));
        if (check_exc(env, "SetByteArrayRegion")) {
            env->DeleteLocalRef(array);
            return -1;
        }
        std::unique_lock<std::mutex> lock(mu_);
        write_done_ = false;
        write_status_ = -1;
        write_seq_ = -1;
        lock.unlock();
        const jint seq = env->CallIntMethod(ble_, m_write_, handle_,
                                            static_cast<jint>(kind), array);
        env->DeleteLocalRef(array);
        if (check_exc(env, "BleLink.write") || seq < 0) {
            return -1;
        }
        lock.lock();
        write_seq_ = seq;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(6000);
        while (!write_done_) {
            if (link_lost_) {
                return -1;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return -1;
            }
            cv_.wait_for(lock, std::chrono::milliseconds(50));
        }
        return write_status_ == 0 ? 0 : -1;
    }

    int read(uint8_t *buf, size_t cap, uint32_t timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mu_);
        if (buf == nullptr || cap == 0) {
            return -1;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        for (;;) {
            if (!frames_.empty()) {
                std::vector<uint8_t> frame = std::move(frames_.front());
                frames_.pop_front();
                const size_t n = std::min(cap, frame.size());
                std::memcpy(buf, frame.data(), n);
                return static_cast<int>(n);
            }
            if (link_lost_) {
                return -1;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return 0;
            }
            cv_.wait_for(lock, std::chrono::milliseconds(50));
        }
    }

    bool is_bonded()
    {
        if (!java_ready_) {
            return false;
        }
        JNIEnv *env = env_for_worker();
        if (env == nullptr) {
            return false;
        }
        const jboolean bonded =
            env->CallBooleanMethod(ble_, m_is_bonded_, handle_);
        return !check_exc(env, "BleLink.isBonded") && bonded == JNI_TRUE;
    }

    int pair()
    {
        if (!java_ready_) {
            return 0;
        }
        if (is_bonded()) {
            return 1;
        }
        JNIEnv *env = env_for_worker();
        if (env == nullptr) {
            return 0;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            bond_done_ = false;
            bond_ok_ = 0;
            pairing_ = true;
        }
        const jboolean started = env->CallBooleanMethod(ble_, m_pair_, handle_);
        if (check_exc(env, "BleLink.pair") || started != JNI_TRUE) {
            std::lock_guard<std::mutex> lock(mu_);
            pairing_ = false;
            return 0;
        }
        std::unique_lock<std::mutex> lock(mu_);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(35);
        while (!bond_done_) {
            if (std::chrono::steady_clock::now() >= deadline) {
                pairing_ = false;
                error_ = "bond timed out";
                return 0;
            }
            cv_.wait_for(lock, std::chrono::milliseconds(100));
        }
        return bond_ok_;
    }

    int write_link_loss_alert(uint8_t level)
    {
        if (!java_ready_) {
            return -1;
        }
        JNIEnv *env = env_for_worker();
        if (env == nullptr) {
            return -1;
        }
        const jboolean ok = env->CallBooleanMethod(
            ble_, m_lla_, handle_, static_cast<jint>(level));
        if (check_exc(env, "BleLink.writeLinkLossAlert")) {
            return -1;
        }
        return ok == JNI_TRUE ? 0 : -1;
    }

    jlong handle_ = 0;
    jobject ble_ = nullptr;
    jclass ble_class_ = nullptr;
    bool java_ready_ = false;
    bool closed_ = false;

    jmethodID m_set_handle_ = nullptr;
    jmethodID m_connect_ = nullptr;
    jmethodID m_connect_to_ = nullptr;
    jmethodID m_start_scan_ = nullptr;
    jmethodID m_stop_scan_ = nullptr;
    jmethodID m_write_ = nullptr;
    jmethodID m_pair_ = nullptr;
    jmethodID m_lla_ = nullptr;
    jmethodID m_is_bonded_ = nullptr;
    jmethodID m_disconnect_ = nullptr;
    jmethodID m_request_permissions_ = nullptr;
    jmethodID m_open_settings_ = nullptr;
    jmethodID m_open_location_settings_ = nullptr;
    jmethodID m_enable_bluetooth_ = nullptr;
    jmethodID m_open_notification_settings_ = nullptr;
    jmethodID m_stop_phone_finder_ = nullptr;
    jmethodID m_notification_access_ = nullptr;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<WatchDevice> devices_;
    std::deque<std::vector<uint8_t>> frames_;
    bool scanning_ = false;
    bool permission_missing_ = false;
    bool bluetooth_off_ = false;
    bool location_off_ = false;
    bool pairing_ = false;
    bool link_lost_ = false;
    bool connected_ = false;
    bool connect_done_ = false;
    bool connect_ok_ = false;
    bool write_done_ = false;
    int write_seq_ = -1;
    int write_status_ = -1;
    bool bond_done_ = false;
    int bond_ok_ = 0;
    std::string address_;
    std::string name_;
    std::string error_;
    std::string target_address_;
    const uint32_t connect_timeout_ms_ = 20000;

    WatchLinkCancel cancel_ = nullptr;
    void *cancel_ctx_ = nullptr;

    wc_transport transport_{};
};

void AndroidWatchLink::close()
{
    if (closed_) {
        return;
    }
    closed_ = true;
    {
        std::lock_guard<std::mutex> lock(g_registry_mu);
        g_registry.erase(handle_);
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        link_lost_ = true;
        cv_.notify_all();
    }
    if (java_ready_) {
        JNIEnv *env = env_for_worker();
        if (env != nullptr) {
            env->CallVoidMethod(ble_, m_disconnect_, handle_);
            check_exc(env, "BleLink.disconnect");
            if (ble_ != nullptr) {
                env->DeleteGlobalRef(ble_);
                ble_ = nullptr;
            }
            if (ble_class_ != nullptr) {
                env->DeleteGlobalRef(ble_class_);
                ble_class_ = nullptr;
            }
        }
    }
    java_ready_ = false;
}

} // namespace

bool platform_supports_scan()
{
    return true;
}

bool platform_take_notification(PhoneNotification &out)
{
    std::lock_guard<std::mutex> lock(g_notification_mu);
    if (g_notifications.empty()) {
        return false;
    }
    out = std::move(g_notifications.front());
    g_notifications.pop_front();
    return true;
}

void android_link_set_app(android_app *app)
{
    if (app == nullptr || app->activity == nullptr ||
        app->activity->vm == nullptr) {
        return;
    }
    JavaVM *vm = app->activity->vm;
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) ==
        JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            return;
        }
    }
    if (env == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_global_mu);
    if (g_activity != nullptr) {
        env->DeleteGlobalRef(g_activity);
    }
    g_activity = env->NewGlobalRef(app->activity->clazz);
    g_vm = vm;
}

} // namespace wristctl::gui

// ── JNI entry points ────────────────────────────────────────────────────────
// Names are derived from dev.wristctl.BleLink; the class is loaded from native
// code through the activity's classloader.
extern "C" {

JNIEXPORT void JNICALL
Java_dev_wristctl_WatchNotificationListener_nativeOnNotification(
    JNIEnv *env, jobject, jint type, jstring app, jstring title, jstring text)
{
    wristctl::gui::PhoneNotification notification;
    notification.type = static_cast<uint8_t>(std::clamp(type, 0, 3));
    notification.app = wristctl::gui::to_string(env, app);
    notification.title = wristctl::gui::to_string(env, title);
    notification.text = wristctl::gui::to_string(env, text);
    if (notification.app.empty()) {
        return;
    }
    if (notification.title.size() > 1024) {
        notification.title.resize(1024);
    }
    if (notification.text.size() > 1024) {
        notification.text.resize(1024);
    }
    std::lock_guard<std::mutex> lock(wristctl::gui::g_notification_mu);
    auto &queue = wristctl::gui::g_notifications;
    if (queue.size() >= 32) {
        queue.pop_front();
    }
    queue.push_back(std::move(notification));
}

JNIEXPORT void JNICALL
Java_dev_wristctl_BleLink_nativeOnConnected(JNIEnv *env, jobject, jlong handle,
                                            jstring address, jstring name)
{
    wristctl::gui::dispatch(handle, [&](auto &link) {
        link.on_connected(env, address, name);
    });
}

JNIEXPORT void JNICALL
Java_dev_wristctl_BleLink_nativeOnDisconnected(JNIEnv *, jobject, jlong handle,
                                               jint status)
{
    wristctl::gui::dispatch(handle, [&](auto &link) {
        link.on_disconnected(status);
    });
}

JNIEXPORT void JNICALL
Java_dev_wristctl_BleLink_nativeOnFrame(JNIEnv *env, jobject, jlong handle,
                                        jbyteArray data)
{
    wristctl::gui::dispatch(handle, [&](auto &link) {
        link.on_frame(env, data);
    });
}

JNIEXPORT void JNICALL
Java_dev_wristctl_BleLink_nativeOnWrite(JNIEnv *, jobject, jlong handle,
                                        jint seq, jint status)
{
    wristctl::gui::dispatch(handle, [&](auto &link) {
        link.on_write(seq, status);
    });
}

JNIEXPORT void JNICALL
Java_dev_wristctl_BleLink_nativeOnBond(JNIEnv *, jobject, jlong handle,
                                       jint bonded)
{
    wristctl::gui::dispatch(handle, [&](auto &link) {
        link.on_bond(bonded);
    });
}

JNIEXPORT void JNICALL
Java_dev_wristctl_BleLink_nativeOnError(JNIEnv *env, jobject, jlong handle,
                                        jstring message)
{
    wristctl::gui::dispatch(handle, [&](auto &link) {
        link.on_error(env, message);
    });
}

JNIEXPORT void JNICALL
Java_dev_wristctl_BleLink_nativeOnScanState(JNIEnv *, jobject, jlong handle,
                                            jint state)
{
    wristctl::gui::dispatch(handle, [&](auto &link) {
        link.on_scan_state(state);
    });
}

JNIEXPORT void JNICALL
Java_dev_wristctl_BleLink_nativeOnDevice(JNIEnv *env, jobject, jlong handle,
                                         jstring name, jstring address,
                                         jint rssi, jboolean bonded)
{
    wristctl::gui::dispatch(handle, [&](auto &link) {
        link.on_device(env, name, address, rssi, bonded == JNI_TRUE);
    });
}

} // extern "C"

namespace wristctl::gui {
namespace {

std::unique_ptr<WatchLink> make_android_link()
{
    return std::make_unique<AndroidWatchLink>();
}

} // namespace

std::unique_ptr<WatchLink> make_watch_link()
{
    return make_android_link();
}

} // namespace wristctl::gui
