#define _POSIX_C_SOURCE 200809L

#include "wristctl/bluez.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <systemd/sd-bus.h>

#define BLUEZ_DEST "org.bluez"
#define DBUS_OM_IFACE "org.freedesktop.DBus.ObjectManager"
#define DBUS_PROP_IFACE "org.freedesktop.DBus.Properties"
#define ADAPTER_IFACE "org.bluez.Adapter1"
#define DEVICE_IFACE "org.bluez.Device1"
#define GATT_SERVICE_IFACE "org.bluez.GattService1"
#define GATT_CHAR_IFACE "org.bluez.GattCharacteristic1"
#define AGENT_IFACE "org.bluez.Agent1"
#define AGENT_MANAGER_IFACE "org.bluez.AgentManager1"
#define AGENT_MANAGER_PATH "/org/bluez"
#define AGENT_PATH "/org/wristctl/agent"
#define AGENT_CAPABILITY "KeyboardDisplay"

#define CASIO_SERVICE_UUID "26eb000d-b012-49a8-b1f8-394fb2032b0f"
#define REQUEST_UUID "26eb002c-b012-49a8-b1f8-394fb2032b0f"
#define SETTINGS_UUID "26eb002d-b012-49a8-b1f8-394fb2032b0f"
#define NOTIFY_UUID "26eb0030-b012-49a8-b1f8-394fb2032b0f"
#define LINK_LOSS_ALERT_UUID "00002a06-0000-1000-8000-00805f9b34fb"

#define MAX_NOTIFY_CHARS 8
#define MAX_GATT_CHARS 32
#define RING_FRAMES 64
#define PATH_MAX_ 256
#define UUID_MAX_ 64

typedef struct {
    char uuid[UUID_MAX_];
    char device[PATH_MAX_];
    char service[PATH_MAX_];
    char address[32];
    char name[64];
    char alias[64];
    bool notify;
    bool indicate;
} obj_props;

struct wc_bluez {
    sd_bus *bus;
    char adapter_path[PATH_MAX_];
    char device_path[PATH_MAX_];
    char address[32];
    char name[64];
    char request_path[PATH_MAX_];
    char settings_path[PATH_MAX_];
    char notify_write_path[PATH_MAX_];
    char notify_paths[MAX_NOTIFY_CHARS][PATH_MAX_];
    size_t notify_count;
    char lla_path[PATH_MAX_];
    char error[256];
    sd_bus_slot *props_slot;
    sd_bus_slot *agent_slot;
    bool agent_registered;
    bool link_lost;
    wc_cancel_fn cancel;
    void *cancel_ctx;
    wc_transport transport;

    struct {
        uint8_t data[WC_FRAME_MAX];
        uint8_t len;
    } ring[RING_FRAMES];
    size_t head;
    size_t count;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t bluez_now_ms(void *ctx)
{
    (void)ctx;
    return now_ms();
}

static void set_error(wc_bluez *bluez, const char *what,
                      const sd_bus_error *error)
{
    const char *detail = "unknown error";
    if (error != NULL) {
        if (error->message != NULL) {
            detail = error->message;
        } else if (error->name != NULL) {
            detail = error->name;
        }
    }
    snprintf(bluez->error, sizeof bluez->error, "%s: %s", what, detail);
}

static void clear_error(wc_bluez *bluez)
{
    bluez->error[0] = '\0';
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (cap == 0) {
        return;
    }
    if (src != NULL) {
        while (i + 1 < cap && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static bool ci_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool ci_contains(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL || *needle == '\0') {
        return false;
    }
    for (const char *start = haystack; *start != '\0'; start++) {
        const char *h = start;
        const char *n = needle;
        while (*h != '\0' && *n != '\0' &&
               tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++;
            n++;
        }
        if (*n == '\0') {
            return true;
        }
    }
    return false;
}

static bool name_matches(const char *name, const char *needle)
{
    static const char *const prefixes[] = {
        "CASIO", "GBX", "GBD", "GPR", "GWF", "GST", "GMW", "MRG", "DW-", "GM-",
    };
    if (name == NULL || *name == '\0') {
        return false;
    }
    if (needle != NULL && *needle != '\0') {
        return ci_contains(name, needle);
    }
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        size_t len = strlen(prefixes[i]);
        bool match = true;
        for (size_t j = 0; j < len; j++) {
            if (name[j] == '\0' ||
                toupper((unsigned char)name[j]) != prefixes[i][j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

static int read_props(sd_bus_message *msg, obj_props *props)
{
    int r;
    while ((r = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
        const char *key = NULL;
        if (sd_bus_message_read(msg, "s", &key) < 0) {
            return -1;
        }
        char type = 0;
        const char *sig = NULL;
        if (sd_bus_message_peek_type(msg, &type, &sig) < 0) {
            return -1;
        }
        const bool variant = type == 'v' && sig != NULL;
        const char *value = NULL;
        if (variant && strcmp(key, "UUID") == 0 && strcmp(sig, "s") == 0) {
            if (sd_bus_message_read(msg, "v", "s", &value) >= 0) {
                copy_str(props->uuid, sizeof props->uuid, value);
            }
        } else if (variant && strcmp(key, "Address") == 0 &&
                   strcmp(sig, "s") == 0) {
            if (sd_bus_message_read(msg, "v", "s", &value) >= 0) {
                copy_str(props->address, sizeof props->address, value);
            }
        } else if (variant && strcmp(key, "Name") == 0 &&
                   strcmp(sig, "s") == 0) {
            if (sd_bus_message_read(msg, "v", "s", &value) >= 0) {
                copy_str(props->name, sizeof props->name, value);
            }
        } else if (variant && strcmp(key, "Alias") == 0 &&
                   strcmp(sig, "s") == 0) {
            if (sd_bus_message_read(msg, "v", "s", &value) >= 0) {
                copy_str(props->alias, sizeof props->alias, value);
            }
        } else if (variant && strcmp(key, "Device") == 0 &&
                   strcmp(sig, "o") == 0) {
            if (sd_bus_message_read(msg, "v", "o", &value) >= 0) {
                copy_str(props->device, sizeof props->device, value);
            }
        } else if (variant && strcmp(key, "Service") == 0 &&
                   strcmp(sig, "o") == 0) {
            if (sd_bus_message_read(msg, "v", "o", &value) >= 0) {
                copy_str(props->service, sizeof props->service, value);
            }
        } else if (variant && strcmp(key, "Flags") == 0 &&
                   strcmp(sig, "as") == 0) {
            /* Flags is an array inside a variant: enter the variant, then the
             * array. Reading "s" directly from the variant fails with EBADMSG. */
            int entered_variant =
                sd_bus_message_enter_container(msg, 'v', "as");
            if (entered_variant > 0) {
                int entered_array = sd_bus_message_enter_container(msg, 'a', "s");
                if (entered_array > 0) {
                    const char *flag = NULL;
                    int read_rc;
                    while ((read_rc = sd_bus_message_read(msg, "s", &flag)) >
                           0) {
                        if (strcmp(flag, "notify") == 0) {
                            props->notify = true;
                        } else if (strcmp(flag, "indicate") == 0) {
                            props->indicate = true;
                        }
                    }
                    if (read_rc < 0 ||
                        sd_bus_message_exit_container(msg) < 0) {
                        return -1;
                    }
                } else if (entered_array < 0) {
                    return -1;
                }
                if (sd_bus_message_exit_container(msg) < 0) {
                    return -1;
                }
            } else if (entered_variant < 0) {
                return -1;
            }
        } else if (sd_bus_message_skip(msg, "v") < 0) {
            if (getenv("WRISTCTL_DEBUG") != NULL) {
                fprintf(stderr, "skip failed key=%s sig=%s\n", key,
                        sig != NULL ? sig : "(null)");
            }
            return -1;
        }
        if (sd_bus_message_exit_container(msg) < 0) {
            if (getenv("WRISTCTL_DEBUG") != NULL) {
                fprintf(stderr, "entry exit failed key=%s\n", key);
            }
            return -1;
        }
    }
    return r == 0 ? 0 : -1;
}

typedef void (*object_cb)(const char *path, const char *iface,
                          const obj_props *props, void *ctx);

static int foreach_object(wc_bluez *bluez, object_cb cb, void *ctx)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(bluez->bus, BLUEZ_DEST, "/", DBUS_OM_IFACE,
                               "GetManagedObjects", &error, &reply, "");
    if (r < 0) {
        set_error(bluez, "GetManagedObjects", &error);
        sd_bus_error_free(&error);
        return -1;
    }

    r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
    if (r < 0) {
        sd_bus_message_unref(reply);
        copy_str(bluez->error, sizeof bluez->error, "malformed GetManagedObjects");
        return -1;
    }

    while ((r = sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}")) > 0) {
        const char *path = NULL;
        if (sd_bus_message_read(reply, "o", &path) < 0) {
            break;
        }
        if (sd_bus_message_enter_container(reply, 'a', "{sa{sv}}") < 0) {
            break;
        }
        int r2;
        while ((r2 = sd_bus_message_enter_container(reply, 'e', "sa{sv}")) > 0) {
            const char *iface = NULL;
            if (sd_bus_message_read(reply, "s", &iface) < 0) {
                break;
            }
            if (sd_bus_message_enter_container(reply, 'a', "{sv}") < 0) {
                break;
            }
            obj_props props;
            memset(&props, 0, sizeof props);
            if (read_props(reply, &props) < 0) {
                break;
            }
            sd_bus_message_exit_container(reply);
            cb(path, iface, &props, ctx);
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_exit_container(reply);
        sd_bus_message_exit_container(reply);
        if (r2 < 0) {
            break;
        }
    }
    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);
    return 0;
}

/* ------------------------------------------------------------------ scan */

typedef struct {
    const char *needle;
    const char *address;
    char path[PATH_MAX_];
    char address_out[32];
    char name_out[64];
    bool found;
    bool fresh; /* seen during this scan, not resurrected from the cache */
    /* Devices BlueZ already had before the scan. A PropertiesChanged event on
     * one of these means it is advertising again even though no
     * InterfacesAdded will arrive for it. */
    struct {
        char path[PATH_MAX_];
        char address[32];
        char name[64];
    } cached[8];
    size_t cached_count;
} scan_ctx;

static void scan_take_device(scan_ctx *scan, const char *path,
                             const obj_props *props, bool fresh)
{
    const char *name = props->name[0] != '\0' ? props->name : props->alias;
    bool match;
    if (scan->address != NULL && *scan->address != '\0') {
        match = ci_equal(props->address, scan->address);
    } else {
        match = name_matches(name, scan->needle);
    }
    if (!match) {
        return;
    }
    copy_str(scan->path, sizeof scan->path, path);
    copy_str(scan->address_out, sizeof scan->address_out, props->address);
    copy_str(scan->name_out, sizeof scan->name_out, name);
    scan->found = true;
    scan->fresh = fresh;
}

static void find_adapter_cb(const char *path, const char *iface,
                            const obj_props *props, void *ctx)
{
    (void)props;
    wc_bluez *bluez = ctx;
    if (strcmp(iface, ADAPTER_IFACE) == 0 && bluez->adapter_path[0] == '\0') {
        copy_str(bluez->adapter_path, sizeof bluez->adapter_path, path);
    }
}

static void find_device_cb(const char *path, const char *iface,
                           const obj_props *props, void *ctx)
{
    scan_ctx *scan = ctx;
    if (scan->found || strcmp(iface, DEVICE_IFACE) != 0) {
        return;
    }
    scan_take_device(scan, path, props, false);
}

static void collect_cached_cb(const char *path, const char *iface,
                              const obj_props *props, void *ctx)
{
    scan_ctx *scan = ctx;
    const size_t cap = sizeof scan->cached / sizeof scan->cached[0];
    if (strcmp(iface, DEVICE_IFACE) != 0 || scan->cached_count >= cap) {
        return;
    }
    const char *name = props->name[0] != '\0' ? props->name : props->alias;
    bool match;
    if (scan->address != NULL && *scan->address != '\0') {
        match = ci_equal(props->address, scan->address);
    } else {
        match = name_matches(name, scan->needle);
    }
    if (!match) {
        return;
    }
    size_t i = scan->cached_count++;
    copy_str(scan->cached[i].path, sizeof scan->cached[i].path, path);
    copy_str(scan->cached[i].address, sizeof scan->cached[i].address,
             props->address);
    copy_str(scan->cached[i].name, sizeof scan->cached[i].name, name);
}

/* A cached device that starts advertising again emits PropertiesChanged, not
 * InterfacesAdded. Mark it found only when an advertisement property changed,
 * so an unrelated Connected/Trusted update does not trigger a connect. */
static int on_scan_properties(sd_bus_message *msg, void *userdata,
                              sd_bus_error *ret_error)
{
    (void)ret_error;
    scan_ctx *scan = userdata;
    if (scan->found) {
        return 0;
    }
    const char *path = sd_bus_message_get_path(msg);
    const char *iface = NULL;
    if (path == NULL || sd_bus_message_read(msg, "s", &iface) < 0) {
        return 0;
    }
    if (strcmp(iface, DEVICE_IFACE) != 0) {
        return 0;
    }

    size_t match = scan->cached_count;
    for (size_t i = 0; i < scan->cached_count; i++) {
        if (strcmp(scan->cached[i].path, path) == 0) {
            match = i;
            break;
        }
    }
    if (match == scan->cached_count) {
        return 0;
    }

    bool advertising = false;
    if (sd_bus_message_enter_container(msg, 'a', "{sv}") <= 0) {
        return 0;
    }
    int r;
    while ((r = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
        const char *key = NULL;
        if (sd_bus_message_read(msg, "s", &key) < 0) {
            break;
        }
        if (strcmp(key, "RSSI") == 0 || strcmp(key, "AdvertisingData") == 0 ||
            strcmp(key, "ManufacturerData") == 0 ||
            strcmp(key, "ServiceData") == 0 || strcmp(key, "UUIDs") == 0) {
            advertising = true;
        }
        if (sd_bus_message_skip(msg, "v") < 0) {
            break;
        }
        sd_bus_message_exit_container(msg);
    }
    sd_bus_message_exit_container(msg);
    if (!advertising) {
        return 0;
    }

    copy_str(scan->path, sizeof scan->path, scan->cached[match].path);
    copy_str(scan->address_out, sizeof scan->address_out,
             scan->cached[match].address);
    copy_str(scan->name_out, sizeof scan->name_out, scan->cached[match].name);
    scan->found = true;
    scan->fresh = true;
    return 0;
}

static int on_interfaces_added(sd_bus_message *msg, void *userdata,
                               sd_bus_error *ret_error)
{
    (void)ret_error;
    scan_ctx *scan = userdata;
    const char *path = NULL;
    if (sd_bus_message_read(msg, "o", &path) < 0) {
        return 0;
    }
    if (sd_bus_message_enter_container(msg, 'a', "{sa{sv}}") < 0) {
        return 0;
    }
    int r;
    while ((r = sd_bus_message_enter_container(msg, 'e', "sa{sv}")) > 0) {
        const char *iface = NULL;
        if (sd_bus_message_read(msg, "s", &iface) < 0) {
            break;
        }
        if (sd_bus_message_enter_container(msg, 'a', "{sv}") < 0) {
            break;
        }
        obj_props props;
        memset(&props, 0, sizeof props);
        if (read_props(msg, &props) < 0) {
            break;
        }
        sd_bus_message_exit_container(msg);
        if (strcmp(iface, DEVICE_IFACE) == 0) {
            scan_take_device(scan, path, &props, true);
        }
        sd_bus_message_exit_container(msg);
    }
    sd_bus_message_exit_container(msg);
    return 0;
}

static bool cancel_requested(const wc_bluez *bluez)
{
    return bluez->cancel != NULL && bluez->cancel(bluez->cancel_ctx);
}

/* Waits for D-Bus events until the deadline. Returns false when the caller
 * asked to cancel or the bus failed; both callers treat it as abort. */
static bool pump_for(wc_bluez *bluez, uint64_t deadline)
{
    for (;;) {
        int r = sd_bus_process(bluez->bus, NULL);
        if (r < 0) {
            return false;
        }
        if (r > 0) {
            continue;
        }
        uint64_t now = now_ms();
        if (now >= deadline) {
            return true;
        }
        if (cancel_requested(bluez)) {
            return false;
        }
        uint64_t slice = deadline - now;
        if (slice > 100) {
            slice = 100;
        }
        if (sd_bus_wait(bluez->bus, slice * 1000u) < 0) {
            return false;
        }
    }
}

static int append_filter_string(sd_bus_message *msg, const char *key,
                                const char *value)
{
    if (sd_bus_message_open_container(msg, 'e', "sv") < 0) {
        return -1;
    }
    if (sd_bus_message_append(msg, "s", key) < 0) {
        return -1;
    }
    if (sd_bus_message_open_container(msg, 'v', "s") < 0) {
        return -1;
    }
    if (sd_bus_message_append(msg, "s", value) < 0) {
        return -1;
    }
    sd_bus_message_close_container(msg);
    return sd_bus_message_close_container(msg);
}

static int append_filter_bool(sd_bus_message *msg, const char *key, int value)
{
    if (sd_bus_message_open_container(msg, 'e', "sv") < 0) {
        return -1;
    }
    if (sd_bus_message_append(msg, "s", key) < 0) {
        return -1;
    }
    if (sd_bus_message_open_container(msg, 'v', "b") < 0) {
        return -1;
    }
    if (sd_bus_message_append(msg, "b", value) < 0) {
        return -1;
    }
    sd_bus_message_close_container(msg);
    return sd_bus_message_close_container(msg);
}

/* Restrict discovery to LE. Best-effort: older BlueZ or an adapter that is
 * still powering up may reject the filter, and the name match still works. */
static void set_le_discovery_filter(wc_bluez *bluez)
{
    sd_bus_message *msg = NULL;
    if (sd_bus_message_new_method_call(bluez->bus, &msg, BLUEZ_DEST,
                                       bluez->adapter_path, ADAPTER_IFACE,
                                       "SetDiscoveryFilter") < 0) {
        return;
    }
    if (sd_bus_message_open_container(msg, 'a', "{sv}") < 0 ||
        append_filter_string(msg, "Transport", "le") < 0 ||
        append_filter_bool(msg, "DuplicateData", 1) < 0 ||
        sd_bus_message_close_container(msg) < 0) {
        sd_bus_message_unref(msg);
        return;
    }
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_call(bluez->bus, msg, 0, &error, NULL);
    sd_bus_error_free(&error);
    sd_bus_message_unref(msg);
}

static int scan_for_device(wc_bluez *bluez, scan_ctx *scan, uint32_t timeout_ms)
{
    if (bluez->adapter_path[0] == '\0') {
        if (foreach_object(bluez, find_adapter_cb, bluez) < 0) {
            return -1;
        }
    }
    if (bluez->adapter_path[0] == '\0') {
        copy_str(bluez->error, sizeof bluez->error, "no Bluetooth adapter found");
        return -1;
    }

    sd_bus_slot *slot = NULL;
    int r = sd_bus_add_match(
        bluez->bus, &slot,
        "type='signal',sender='org.bluez',"
        "interface='org.freedesktop.DBus.ObjectManager',"
        "member='InterfacesAdded'",
        on_interfaces_added, scan);
    if (r < 0) {
        copy_str(bluez->error, sizeof bluez->error, "cannot subscribe to BlueZ");
        return -1;
    }

    sd_bus_slot *props_slot = NULL;
    r = sd_bus_add_match(
        bluez->bus, &props_slot,
        "type='signal',sender='org.bluez',"
        "interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged'",
        on_scan_properties, scan);
    if (r < 0) {
        sd_bus_slot_unref(slot);
        copy_str(bluez->error, sizeof bluez->error,
                 "cannot subscribe to BlueZ property changes");
        return -1;
    }

    /* Remember the matching devices BlueZ already knows about; an advertisement
     * from one of them arrives as PropertiesChanged, not InterfacesAdded. */
    scan->cached_count = 0;
    (void)foreach_object(bluez, collect_cached_cb, scan);

    set_le_discovery_filter(bluez);

    sd_bus_error error = SD_BUS_ERROR_NULL;
    r = sd_bus_call_method(bluez->bus, BLUEZ_DEST, bluez->adapter_path,
                           ADAPTER_IFACE, "StartDiscovery", &error, NULL, "");
    if (r < 0) {
        /* A discovery already in progress is not fatal. */
        clear_error(bluez);
    }
    sd_bus_error_free(&error);

    bool cancelled = false;
    uint64_t deadline = now_ms() + (timeout_ms != 0 ? timeout_ms : 12000);
    while (!scan->found && now_ms() < deadline) {
        if (cancel_requested(bluez)) {
            cancelled = true;
            break;
        }
        if (sd_bus_process(bluez->bus, NULL) < 0) {
            break;
        }
        uint64_t now = now_ms();
        if (now >= deadline) {
            break;
        }
        uint64_t slice = deadline - now;
        if (slice > 100) {
            slice = 100;
        }
        if (sd_bus_wait(bluez->bus, slice * 1000u) < 0) {
            break;
        }
    }

    error = SD_BUS_ERROR_NULL;
    sd_bus_call_method(bluez->bus, BLUEZ_DEST, bluez->adapter_path,
                       ADAPTER_IFACE, "StopDiscovery", &error, NULL, "");
    sd_bus_error_free(&error);
    sd_bus_slot_unref(props_slot);
    sd_bus_slot_unref(slot);

    if (cancelled) {
        copy_str(bluez->error, sizeof bluez->error, "cancelled");
        return -1;
    }

    /* A device BlueZ already knew about may not generate InterfacesAdded.
     * Fall back to the object tree, but only after the advertisement window:
     * the watch regenerates its address, so a fresh signal is preferred. */
    if (!scan->found) {
        foreach_object(bluez, find_device_cb, scan);
    }

    if (!scan->found) {
        copy_str(bluez->error, sizeof bluez->error,
                 "watch not found; free it from the phone and press a button");
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------- connect */

static int wait_services_resolved(wc_bluez *bluez, uint32_t timeout_ms)
{
    uint64_t deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message *reply = NULL;
        int r = sd_bus_get_property(bluez->bus, BLUEZ_DEST, bluez->device_path,
                                    DEVICE_IFACE, "ServicesResolved", &error,
                                    &reply, "b");
        if (r >= 0) {
            int resolved = 0;
            sd_bus_message_read(reply, "b", &resolved);
            sd_bus_message_unref(reply);
            sd_bus_error_free(&error);
            if (resolved) {
                return 0;
            }
        } else {
            sd_bus_error_free(&error);
        }
        if (!pump_for(bluez, now_ms() + 100)) {
            copy_str(bluez->error, sizeof bluez->error,
                     cancel_requested(bluez) ? "cancelled"
                                             : "D-Bus error while waiting for GATT");
            return -1;
        }
    }
    copy_str(bluez->error, sizeof bluez->error, "GATT services did not resolve");
    return -1;
}

typedef struct {
    char service[PATH_MAX_];
    char lla[PATH_MAX_];
    struct {
        char path[PATH_MAX_];
        char service[PATH_MAX_];
        char uuid[UUID_MAX_];
        bool notify;
    } chars[MAX_GATT_CHARS];
    size_t count;
} gatt_ctx;

static void find_gatt_cb(const char *path, const char *iface,
                         const obj_props *props, void *ctx)
{
    gatt_ctx *gatt = ctx;
    if (getenv("WRISTCTL_DEBUG") != NULL) {
        fprintf(stderr, "gatt object %s iface=%s uuid=%s service=%s\n", path,
                iface, props->uuid, props->service);
    }
    if (strcmp(iface, GATT_SERVICE_IFACE) == 0) {
        if (strcmp(props->uuid, CASIO_SERVICE_UUID) == 0 &&
            gatt->service[0] == '\0') {
            copy_str(gatt->service, sizeof gatt->service, path);
        }
        return;
    }
    if (strcmp(iface, GATT_CHAR_IFACE) != 0) {
        return;
    }
    if (getenv("WRISTCTL_DEBUG") != NULL) {
        fprintf(stderr, "gatt char %s uuid=%s service=%s notify=%d indicate=%d\n",
                path, props->uuid, props->service, props->notify,
                props->indicate);
    }
    if (strcmp(props->uuid, LINK_LOSS_ALERT_UUID) == 0 &&
        gatt->lla[0] == '\0') {
        copy_str(gatt->lla, sizeof gatt->lla, path);
    }
    if (gatt->count < MAX_GATT_CHARS) {
        size_t i = gatt->count++;
        copy_str(gatt->chars[i].path, sizeof gatt->chars[i].path, path);
        copy_str(gatt->chars[i].service, sizeof gatt->chars[i].service,
                 props->service);
        copy_str(gatt->chars[i].uuid, sizeof gatt->chars[i].uuid, props->uuid);
        /* Trust the GATT Flags from BlueZ. The settings characteristic is
         * read/write on the GBX-100; subscribing to it returns NotSupported. */
        gatt->chars[i].notify = props->notify || props->indicate;
    }
}

static void reset_gatt(wc_bluez *bluez)
{
    bluez->request_path[0] = '\0';
    bluez->settings_path[0] = '\0';
    bluez->notify_write_path[0] = '\0';
    bluez->lla_path[0] = '\0';
    bluez->notify_count = 0;
    memset(bluez->notify_paths, 0, sizeof bluez->notify_paths);
}

static int resolve_gatt(wc_bluez *bluez)
{
    reset_gatt(bluez);
    gatt_ctx gatt;
    memset(&gatt, 0, sizeof gatt);
    if (foreach_object(bluez, find_gatt_cb, &gatt) < 0) {
        return -1;
    }
    if (gatt.service[0] == '\0') {
        copy_str(bluez->error, sizeof bluez->error,
                 "Casio service 26eb000d not found");
        return -1;
    }

    for (size_t i = 0; i < gatt.count; i++) {
        if (strcmp(gatt.chars[i].service, gatt.service) != 0) {
            continue;
        }
        if (strcmp(gatt.chars[i].uuid, REQUEST_UUID) == 0) {
            copy_str(bluez->request_path, sizeof bluez->request_path,
                     gatt.chars[i].path);
        } else if (strcmp(gatt.chars[i].uuid, SETTINGS_UUID) == 0) {
            copy_str(bluez->settings_path, sizeof bluez->settings_path,
                     gatt.chars[i].path);
        } else if (strcmp(gatt.chars[i].uuid, NOTIFY_UUID) == 0) {
            copy_str(bluez->notify_write_path, sizeof bluez->notify_write_path,
                     gatt.chars[i].path);
        }
        if (gatt.chars[i].notify &&
            bluez->notify_count < MAX_NOTIFY_CHARS) {
            bool duplicate = false;
            for (size_t j = 0; j < bluez->notify_count; j++) {
                if (strcmp(bluez->notify_paths[j], gatt.chars[i].path) == 0) {
                    duplicate = true;
                }
            }
            if (!duplicate) {
                copy_str(bluez->notify_paths[bluez->notify_count],
                         sizeof bluez->notify_paths[0], gatt.chars[i].path);
                bluez->notify_count++;
            }
        }
    }

    if (bluez->request_path[0] == '\0' || bluez->settings_path[0] == '\0') {
        copy_str(bluez->error, sizeof bluez->error,
                 "request/settings characteristic missing");
        return -1;
    }
    if (bluez->notify_count == 0) {
        copy_str(bluez->error, sizeof bluez->error,
                 "watch exposed no notifying characteristic");
        return -1;
    }
    copy_str(bluez->lla_path, sizeof bluez->lla_path, gatt.lla);
    return 0;
}

/* (Re)start notifications on every notifying characteristic. A reconnect can
 * leave BlueZ reporting the old subscription as active, so AlreadyExists and
 * InProgress are treated as success. */
static int start_notifications(wc_bluez *bluez)
{
    for (size_t i = 0; i < bluez->notify_count; i++) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        int r = sd_bus_call_method(bluez->bus, BLUEZ_DEST,
                                   bluez->notify_paths[i], GATT_CHAR_IFACE,
                                   "StartNotify", &error, NULL, "");
        if (r < 0) {
            const bool already =
                error.name != NULL &&
                (strcmp(error.name, "org.bluez.Error.AlreadyExists") == 0 ||
                 strcmp(error.name, "org.bluez.Error.InProgress") == 0);
            if (!already) {
                set_error(bluez, "StartNotify", &error);
                sd_bus_error_free(&error);
                return -1;
            }
        }
        sd_bus_error_free(&error);
    }
    return 0;
}

static void debug_frame(const char *tag, const uint8_t *data, size_t len);

static void ring_push(wc_bluez *bluez, const uint8_t *data, size_t len)
{
    if (len == 0 || len > WC_FRAME_MAX) {
        return;
    }
    if (bluez->count == RING_FRAMES) {
        bluez->head = (bluez->head + 1) % RING_FRAMES;
        bluez->count--;
    }
    size_t index = (bluez->head + bluez->count) % RING_FRAMES;
    memcpy(bluez->ring[index].data, data, len);
    bluez->ring[index].len = (uint8_t)len;
    bluez->count++;
}

static bool watched_path(const wc_bluez *bluez, const char *path)
{
    if (bluez->device_path[0] != '\0' &&
        strcmp(bluez->device_path, path) == 0) {
        return true;
    }
    for (size_t i = 0; i < bluez->notify_count; i++) {
        if (strcmp(bluez->notify_paths[i], path) == 0) {
            return true;
        }
    }
    return false;
}

static int on_properties_changed(sd_bus_message *msg, void *userdata,
                                 sd_bus_error *ret_error)
{
    (void)ret_error;
    wc_bluez *bluez = userdata;
    const char *path = sd_bus_message_get_path(msg);
    if (path == NULL || !watched_path(bluez, path)) {
        return 0;
    }

    const char *iface = NULL;
    if (sd_bus_message_read(msg, "s", &iface) < 0) {
        return 0;
    }
    const bool device_iface = strcmp(iface, DEVICE_IFACE) == 0;
    if (sd_bus_message_enter_container(msg, 'a', "{sv}") < 0) {
        return 0;
    }
    int r;
    while ((r = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
        const char *key = NULL;
        if (sd_bus_message_read(msg, "s", &key) < 0) {
            break;
        }
        char type = 0;
        const char *sig = NULL;
        if (sd_bus_message_peek_type(msg, &type, &sig) < 0) {
            break;
        }
        if (type == 'v' && sig != NULL && strcmp(key, "Value") == 0 &&
            strcmp(sig, "ay") == 0) {
            if (sd_bus_message_enter_container(msg, 'v', "ay") >= 0) {
                const void *data = NULL;
                size_t size = 0;
                if (sd_bus_message_read_array(msg, 'y', &data, &size) > 0 &&
                    size <= WC_FRAME_MAX) {
                    debug_frame("rx notify:", data, size);
                    ring_push(bluez, data, size);
                }
                sd_bus_message_exit_container(msg);
            } else {
                sd_bus_message_skip(msg, "v");
            }
        } else if (device_iface && type == 'v' && sig != NULL &&
                   strcmp(key, "Connected") == 0 && strcmp(sig, "b") == 0) {
            int connected = 1;
            if (sd_bus_message_enter_container(msg, 'v', "b") >= 0) {
                if (sd_bus_message_read(msg, "b", &connected) >= 0) {
                    bluez->link_lost = connected == 0;
                }
                sd_bus_message_exit_container(msg);
            } else {
                sd_bus_message_skip(msg, "v");
            }
        } else {
            sd_bus_message_skip(msg, "v");
        }
        sd_bus_message_exit_container(msg);
    }
    sd_bus_message_exit_container(msg);
    return 0;
}

/* -------------------------------------------------------------- transport */

static int write_char(wc_bluez *bluez, const char *path, const uint8_t *data,
                      size_t len, const char *type)
{
    sd_bus_message *msg = NULL;
    int r = sd_bus_message_new_method_call(bluez->bus, &msg, BLUEZ_DEST, path,
                                           GATT_CHAR_IFACE, "WriteValue");
    if (r < 0) {
        copy_str(bluez->error, sizeof bluez->error, "cannot build WriteValue");
        return -1;
    }
    r = sd_bus_message_append_array(msg, 'y', data, len);
    if (r >= 0) {
        r = sd_bus_message_open_container(msg, 'a', "{sv}");
    }
    if (r >= 0) {
        r = sd_bus_message_open_container(msg, 'e', "sv");
    }
    if (r >= 0) {
        r = sd_bus_message_append(msg, "s", "type");
    }
    if (r >= 0) {
        r = sd_bus_message_open_container(msg, 'v', "s");
    }
    if (r >= 0) {
        r = sd_bus_message_append(msg, "s", type);
    }
    if (r >= 0) {
        r = sd_bus_message_close_container(msg); /* variant */
    }
    if (r >= 0) {
        r = sd_bus_message_close_container(msg); /* entry */
    }
    if (r >= 0) {
        r = sd_bus_message_close_container(msg); /* array */
    }
    if (r < 0) {
        sd_bus_message_unref(msg);
        copy_str(bluez->error, sizeof bluez->error, "cannot build WriteValue");
        return -1;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    r = sd_bus_call(bluez->bus, msg, 10u * 1000u * 1000u, &error, NULL);
    sd_bus_message_unref(msg);
    if (r < 0) {
        set_error(bluez, "WriteValue", &error);
        sd_bus_error_free(&error);
        return -1;
    }
    clear_error(bluez);
    sd_bus_error_free(&error);
    return 0;
}

static void debug_frame(const char *tag, const uint8_t *data, size_t len)
{
    if (getenv("WRISTCTL_DEBUG") == NULL) {
        return;
    }
    fprintf(stderr, "%s", tag);
    for (size_t i = 0; i < len; i++) {
        fprintf(stderr, " %02x", data[i]);
    }
    fprintf(stderr, "\n");
}

static int bluez_write(void *ctx, wc_write_kind kind, const uint8_t *data,
                       size_t len)
{
    wc_bluez *bluez = ctx;
    const char *path = NULL;
    const char *type = "command";
    switch (kind) {
    case WC_WRITE_REQUEST:
        path = bluez->request_path;
        type = "command";
        break;
    case WC_WRITE_ALL:
        path = bluez->settings_path;
        type = "request";
        break;
    case WC_WRITE_ALL_COMMAND:
        path = bluez->settings_path;
        type = "command";
        break;
    case WC_WRITE_NOTIFICATION:
        path = bluez->notify_write_path;
        type = "command";
        break;
    default:
        copy_str(bluez->error, sizeof bluez->error, "unknown write kind");
        return -1;
    }
    if (path == NULL || *path == '\0') {
        copy_str(bluez->error, sizeof bluez->error,
                 "characteristic not resolved");
        return -1;
    }
    debug_frame(kind == WC_WRITE_REQUEST ? "tx request:"
                                         : "tx settings:",
                data, len);
    int rc = write_char(bluez, path, data, len, type);
    if (rc < 0) {
        debug_frame("write failed:", data, len);
    }
    return rc;
}

static int bluez_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    wc_bluez *bluez = ctx;
    uint64_t deadline = now_ms() + (timeout_ms != 0 ? timeout_ms : 1);
    for (;;) {
        if (bluez->count > 0) {
            size_t index = bluez->head;
            bluez->head = (bluez->head + 1) % RING_FRAMES;
            bluez->count--;
            size_t len = bluez->ring[index].len;
            if (len > cap) {
                copy_str(bluez->error, sizeof bluez->error,
                         "transport buffer too small");
                return -1;
            }
            memcpy(buf, bluez->ring[index].data, len);
            return (int)len;
        }

        int r = sd_bus_process(bluez->bus, NULL);
        if (r < 0) {
            copy_str(bluez->error, sizeof bluez->error, "sd_bus_process failed");
            return -1;
        }
        if (r > 0) {
            continue;
        }
        uint64_t now = now_ms();
        if (now >= deadline) {
            return 0;
        }
        if (sd_bus_wait(bluez->bus, (deadline - now) * 1000u) < 0) {
            copy_str(bluez->error, sizeof bluez->error, "sd_bus_wait failed");
            return -1;
        }
    }
}

static int bluez_link_loss_alert(void *ctx, uint8_t level)
{
    wc_bluez *bluez = ctx;
    if (bluez->lla_path[0] == '\0') {
        return -1;
    }
    return write_char(bluez, bluez->lla_path, &level, 1, "request");
}

/* ---------------------------------------------------------------- pairing */

/* BlueZ asks a registered org.bluez.Agent1 for confirmation. The GBX-100 has
 * a display, so advertise KeyboardDisplay and accept a passkey typed by the
 * user. This is the one interactive step of the initial bring-up; the bond is
 * stored by BlueZ and later connections do not prompt. */
static int agent_check_device(wc_bluez *bluez, const char *device,
                              sd_bus_error *error)
{
    if (device == NULL || bluez->device_path[0] == '\0' ||
        strcmp(device, bluez->device_path) != 0) {
        return sd_bus_error_set_const(error, "org.bluez.Error.Rejected",
                                      "not the configured watch");
    }
    return 0;
}

static bool agent_read_line(const char *prompt, char *buffer, size_t cap)
{
    fprintf(stderr, "%s", prompt);
    fflush(stderr);
    if (fgets(buffer, (int)cap, stdin) == NULL) {
        return false;
    }
    size_t len = strlen(buffer);
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
        buffer[--len] = '\0';
    }
    return true;
}

static int agent_release(sd_bus_message *msg, void *userdata,
                         sd_bus_error *error)
{
    (void)msg;
    (void)userdata;
    (void)error;
    return 0;
}

static int agent_request_pin_code(sd_bus_message *msg, void *userdata,
                                  sd_bus_error *error)
{
    wc_bluez *bluez = userdata;
    const char *device = NULL;
    if (sd_bus_message_read(msg, "o", &device) < 0) {
        return -EINVAL;
    }
    int rc = agent_check_device(bluez, device, error);
    if (rc != 0) {
        return rc;
    }
    char code[32];
    if (!agent_read_line("Enter the PIN shown on the watch: ", code,
                         sizeof code) ||
        code[0] == '\0') {
        return sd_bus_error_set_const(error, "org.bluez.Error.Rejected",
                                      "no PIN entered");
    }
    return sd_bus_reply_method_return(msg, "s", code);
}

static int agent_display_pin_code(sd_bus_message *msg, void *userdata,
                                  sd_bus_error *error)
{
    (void)userdata;
    (void)error;
    const char *device = NULL;
    const char *code = NULL;
    if (sd_bus_message_read(msg, "os", &device, &code) >= 0) {
        fprintf(stderr, "agent: PIN for %s is %s\n", device, code);
    }
    return 0;
}

static int agent_request_passkey(sd_bus_message *msg, void *userdata,
                                 sd_bus_error *error)
{
    wc_bluez *bluez = userdata;
    const char *device = NULL;
    if (sd_bus_message_read(msg, "o", &device) < 0) {
        return -EINVAL;
    }
    int rc = agent_check_device(bluez, device, error);
    if (rc != 0) {
        return rc;
    }
    char line[32];
    if (!agent_read_line("Enter the 6-digit passkey shown on the watch: ",
                         line, sizeof line)) {
        return sd_bus_error_set_const(error, "org.bluez.Error.Rejected",
                                      "no passkey entered");
    }
    char *end = NULL;
    unsigned long passkey = strtoul(line, &end, 10);
    if (end == line || *end != '\0' || passkey > 999999ul) {
        return sd_bus_error_set_const(error, "org.bluez.Error.Rejected",
                                      "invalid passkey");
    }
    return sd_bus_reply_method_return(msg, "u", (uint32_t)passkey);
}

static int agent_display_passkey(sd_bus_message *msg, void *userdata,
                                 sd_bus_error *error)
{
    (void)userdata;
    (void)error;
    const char *device = NULL;
    uint16_t passkey = 0;
    uint32_t entered = 0;
    if (sd_bus_message_read(msg, "oqu", &device, &passkey, &entered) >= 0) {
        fprintf(stderr, "agent: passkey for %s is %06u (entered %u)\n", device,
                passkey, entered);
    }
    return 0;
}

static int agent_request_confirmation(sd_bus_message *msg, void *userdata,
                                      sd_bus_error *error)
{
    wc_bluez *bluez = userdata;
    const char *device = NULL;
    uint32_t passkey = 0;
    if (sd_bus_message_read(msg, "ou", &device, &passkey) < 0) {
        return -EINVAL;
    }
    int rc = agent_check_device(bluez, device, error);
    if (rc != 0) {
        return rc;
    }
    fprintf(stderr,
            "agent: confirm that the watch shows %06u and press its confirm "
            "button\n",
            passkey);
    return 0;
}

static int agent_request_authorization(sd_bus_message *msg, void *userdata,
                                       sd_bus_error *error)
{
    wc_bluez *bluez = userdata;
    const char *device = NULL;
    if (sd_bus_message_read(msg, "o", &device) < 0) {
        return -EINVAL;
    }
    return agent_check_device(bluez, device, error);
}

static int agent_authorize_service(sd_bus_message *msg, void *userdata,
                                   sd_bus_error *error)
{
    wc_bluez *bluez = userdata;
    const char *device = NULL;
    const char *uuid = NULL;
    if (sd_bus_message_read(msg, "os", &device, &uuid) < 0) {
        return -EINVAL;
    }
    (void)uuid;
    return agent_check_device(bluez, device, error);
}

static int agent_cancel(sd_bus_message *msg, void *userdata,
                        sd_bus_error *error)
{
    (void)msg;
    (void)userdata;
    (void)error;
    return 0;
}

static const sd_bus_vtable agent_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "", "", agent_release, 0),
    SD_BUS_METHOD("RequestPinCode", "o", "s", agent_request_pin_code, 0),
    SD_BUS_METHOD("DisplayPinCode", "os", "", agent_display_pin_code, 0),
    SD_BUS_METHOD("RequestPasskey", "o", "u", agent_request_passkey, 0),
    SD_BUS_METHOD("DisplayPasskey", "oqu", "", agent_display_passkey, 0),
    SD_BUS_METHOD("RequestConfirmation", "ou", "",
                  agent_request_confirmation, 0),
    SD_BUS_METHOD("RequestAuthorization", "o", "",
                  agent_request_authorization, 0),
    SD_BUS_METHOD("AuthorizeService", "os", "", agent_authorize_service, 0),
    SD_BUS_METHOD("Cancel", "", "", agent_cancel, 0),
    SD_BUS_VTABLE_END};

static int ensure_agent(wc_bluez *bluez)
{
    if (bluez->agent_registered) {
        return 0;
    }
    int r = sd_bus_add_object_vtable(bluez->bus, &bluez->agent_slot,
                                     AGENT_PATH, AGENT_IFACE, agent_vtable,
                                     bluez);
    if (r < 0) {
        copy_str(bluez->error, sizeof bluez->error,
                 "cannot export the BlueZ pairing agent");
        return -1;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    r = sd_bus_call_method(bluez->bus, BLUEZ_DEST, AGENT_MANAGER_PATH,
                           AGENT_MANAGER_IFACE, "RegisterAgent", &error, NULL,
                           "os", AGENT_PATH, AGENT_CAPABILITY);
    if (r < 0) {
        set_error(bluez, "RegisterAgent", &error);
        sd_bus_error_free(&error);
        sd_bus_slot_unref(bluez->agent_slot);
        bluez->agent_slot = NULL;
        return -1;
    }
    sd_bus_error_free(&error);

    /* Another agent (bluetoothctl, a desktop applet) may be the default. Take
     * over; if that fails the agent is still registered for our own Pair. */
    error = SD_BUS_ERROR_NULL;
    (void)sd_bus_call_method(bluez->bus, BLUEZ_DEST, AGENT_MANAGER_PATH,
                             AGENT_MANAGER_IFACE, "RequestDefaultAgent", &error,
                             NULL, "o", AGENT_PATH);
    sd_bus_error_free(&error);

    bluez->agent_registered = true;
    clear_error(bluez);
    return 0;
}

static void unregister_agent(wc_bluez *bluez)
{
    if (bluez->agent_registered) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        (void)sd_bus_call_method(bluez->bus, BLUEZ_DEST, AGENT_MANAGER_PATH,
                                 AGENT_MANAGER_IFACE, "UnregisterAgent", &error,
                                 NULL, "o", AGENT_PATH);
        sd_bus_error_free(&error);
        bluez->agent_registered = false;
    }
    if (bluez->agent_slot != NULL) {
        sd_bus_slot_unref(bluez->agent_slot);
        bluez->agent_slot = NULL;
    }
}

static int get_bool_prop(wc_bluez *bluez, const char *name, bool *out)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_get_property(bluez->bus, BLUEZ_DEST, bluez->device_path,
                                DEVICE_IFACE, name, &error, &reply, "b");
    if (r < 0) {
        sd_bus_error_free(&error);
        return -1;
    }
    int value = 0;
    r = sd_bus_message_read(reply, "b", &value);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    if (r < 0) {
        return -1;
    }
    *out = value != 0;
    return 0;
}

static void trust_device(wc_bluez *bluez)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    (void)sd_bus_set_property(bluez->bus, BLUEZ_DEST, bluez->device_path,
                              DEVICE_IFACE, "Trusted", &error, "b", 1);
    sd_bus_error_free(&error);
}

/* A fresh bond can make BlueZ drop and re-establish the ACL, which clears the
 * GATT subscription the session is waiting on. Reconnect and re-arm only when
 * the link actually changed. */
static int ensure_link_after_pair(wc_bluez *bluez)
{
    pump_for(bluez, now_ms() + 300);

    bool connected = false;
    if (get_bool_prop(bluez, "Connected", &connected) < 0) {
        return -1;
    }

    bool need_rearm = false;
    if (!connected) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        int r = sd_bus_call_method(bluez->bus, BLUEZ_DEST, bluez->device_path,
                                   DEVICE_IFACE, "Connect", &error, NULL, "");
        if (r < 0) {
            const bool in_progress =
                error.name != NULL &&
                (strcmp(error.name, "org.bluez.Error.InProgress") == 0 ||
                 strcmp(error.name, "org.bluez.Error.AlreadyConnected") == 0);
            if (!in_progress) {
                set_error(bluez, "Connect after Pair", &error);
                sd_bus_error_free(&error);
                return -1;
            }
        }
        sd_bus_error_free(&error);
        need_rearm = true;
    }

    bool resolved = false;
    if (get_bool_prop(bluez, "ServicesResolved", &resolved) < 0 || !resolved) {
        if (wait_services_resolved(bluez, need_rearm ? 15000 : 5000) < 0) {
            return -1;
        }
        need_rearm = true;
    }

    if (need_rearm) {
        if (resolve_gatt(bluez) < 0) {
            return -1;
        }
        if (start_notifications(bluez) < 0) {
            return -1;
        }
    }
    return 0;
}

static int bluez_pair(void *ctx, const char *address)
{
    (void)address;
    wc_bluez *bluez = ctx;
    const bool debug = getenv("WRISTCTL_DEBUG") != NULL;
    if (bluez->device_path[0] == '\0') {
        copy_str(bluez->error, sizeof bluez->error, "no device to pair");
        return 0;
    }

    bool paired = false;
    if (get_bool_prop(bluez, "Paired", &paired) == 0 && paired) {
        if (debug) {
            fprintf(stderr, "pair: already bonded\n");
        }
        return 1;
    }
    if (debug) {
        fprintf(stderr, "pair: registering agent\n");
    }
    if (ensure_agent(bluez) < 0) {
        if (debug) {
            fprintf(stderr, "pair: agent registration failed: %s\n",
                    bluez->error);
        }
        return 0;
    }

    if (debug) {
        fprintf(stderr, "pair: calling Device1.Pair\n");
    }
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(bluez->bus, BLUEZ_DEST, bluez->device_path,
                               DEVICE_IFACE, "Pair", &error, NULL, "");
    if (debug) {
        fprintf(stderr, "pair: Pair returned %d (%s)\n", r,
                error.message != NULL ? error.message : "ok");
    }
    if (r < 0) {
        const bool exists =
            error.name != NULL &&
            strcmp(error.name, "org.bluez.Error.AlreadyExists") == 0;
        if (!exists) {
            set_error(bluez, "Pair", &error);
            sd_bus_error_free(&error);
            return 0;
        }
    }
    sd_bus_error_free(&error);

    paired = false;
    if (get_bool_prop(bluez, "Paired", &paired) < 0 || !paired) {
        copy_str(bluez->error, sizeof bluez->error,
                 "Pair returned without bonding the watch");
        return 0;
    }
    trust_device(bluez);

    /* The bond is what the caller asked for. If re-arming fails, keep the bond
     * and leave the transport error for the caller to print. */
    if (ensure_link_after_pair(bluez) == 0) {
        clear_error(bluez);
    }
    return 1;
}

static void bluez_close(void *ctx)
{
    wc_bluez *bluez = ctx;
    for (size_t i = 0; i < bluez->notify_count; i++) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_call_method(bluez->bus, BLUEZ_DEST, bluez->notify_paths[i],
                           GATT_CHAR_IFACE, "StopNotify", &error, NULL, "");
        sd_bus_error_free(&error);
    }
    if (bluez->device_path[0] != '\0') {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_call_method(bluez->bus, BLUEZ_DEST, bluez->device_path,
                           DEVICE_IFACE, "Disconnect", &error, NULL, "");
        sd_bus_error_free(&error);
    }
}

wc_bluez *wc_bluez_new(void)
{
    wc_bluez *bluez = calloc(1, sizeof *bluez);
    if (bluez == NULL) {
        return NULL;
    }
    if (sd_bus_open_system(&bluez->bus) < 0) {
        free(bluez);
        return NULL;
    }
    bluez->transport.ctx = bluez;
    bluez->transport.write = bluez_write;
    bluez->transport.read = bluez_read;
    bluez->transport.write_link_loss_alert = bluez_link_loss_alert;
    bluez->transport.pair = bluez_pair;
    bluez->transport.close = bluez_close;
    bluez->transport.now_ms = bluez_now_ms;
    bluez->transport.address = bluez->address;
    return bluez;
}

static bool auth_error(const sd_bus_error *error)
{
    if (error == NULL) {
        return false;
    }
    if (error->name != NULL &&
        (strcmp(error->name, "org.bluez.Error.AuthenticationFailed") == 0 ||
         strcmp(error->name, "org.bluez.Error.AuthenticationRejected") == 0 ||
         strcmp(error->name, "org.bluez.Error.AuthenticationCanceled") == 0 ||
         strcmp(error->name, "org.bluez.Error.AuthenticationTimeout") == 0)) {
        return true;
    }
    /* BlueZ sometimes wraps the kernel status in org.bluez.Error.Failed with
     * "Authentication Failed" as the message. */
    return error->message != NULL && ci_contains(error->message, "auth");
}

static int remove_device_path(wc_bluez *bluez, const char *path)
{
    if (bluez->adapter_path[0] == '\0') {
        if (foreach_object(bluez, find_adapter_cb, bluez) < 0) {
            return -1;
        }
    }
    if (bluez->adapter_path[0] == '\0') {
        copy_str(bluez->error, sizeof bluez->error,
                 "no Bluetooth adapter found");
        return -1;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(bluez->bus, BLUEZ_DEST, bluez->adapter_path,
                               ADAPTER_IFACE, "RemoveDevice", &error, NULL, "o",
                               path);
    if (r < 0) {
        const bool missing =
            error.name != NULL &&
            strcmp(error.name, "org.bluez.Error.DoesNotExist") == 0;
        if (!missing) {
            set_error(bluez, "RemoveDevice", &error);
            sd_bus_error_free(&error);
            return -1;
        }
    }
    sd_bus_error_free(&error);
    return 0;
}

static int connect_and_resolve(wc_bluez *bluez, const char *name,
                               const char *address, uint32_t scan_timeout_ms,
                               bool *remove_and_retry)
{
    *remove_and_retry = false;
    bluez->link_lost = false;

    if (cancel_requested(bluez)) {
        copy_str(bluez->error, sizeof bluez->error, "cancelled");
        return WC_SESSION_ERR_TIMEOUT;
    }

    scan_ctx scan;
    memset(&scan, 0, sizeof scan);
    scan.needle = name;
    scan.address = address;

    /* An explicit address is a caller decision; otherwise always scan, because
     * the GBX-100 regenerates its random address on power-up. */
    if (address != NULL && *address != '\0') {
        if (foreach_object(bluez, find_device_cb, &scan) < 0) {
            return WC_SESSION_ERR_TRANSPORT;
        }
    }
    if (!scan.found) {
        if (scan_for_device(bluez, &scan, scan_timeout_ms) < 0) {
            return WC_SESSION_ERR_TRANSPORT;
        }
    }
    copy_str(bluez->device_path, sizeof bluez->device_path, scan.path);
    copy_str(bluez->address, sizeof bluez->address, scan.address_out);
    copy_str(bluez->name, sizeof bluez->name, scan.name_out);

    /* The tree fallback can resurrect a bonded device that is not advertising.
     * Connecting to it blocks in BlueZ for a long timeout and makes the retry
     * loop slow. Only accept such a device when the link is already up. */
    const bool explicit_address = address != NULL && *address != '\0';
    if (!scan.fresh && !explicit_address) {
        bool connected = false;
        if (get_bool_prop(bluez, "Connected", &connected) < 0 || !connected) {
            copy_str(bluez->error, sizeof bluez->error,
                     "watch not found; free it from the phone and press a button");
            return WC_SESSION_ERR_TRANSPORT;
        }
    }

    if (cancel_requested(bluez)) {
        copy_str(bluez->error, sizeof bluez->error, "cancelled");
        return WC_SESSION_ERR_TIMEOUT;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(bluez->bus, BLUEZ_DEST, bluez->device_path,
                               DEVICE_IFACE, "Connect", &error, NULL, "");
    if (r < 0) {
        /* BlueZ reports InProgress when another connection attempt is already
         * underway; the caller still wants to wait for ServicesResolved. */
        const bool in_progress =
            error.name != NULL &&
            (strcmp(error.name, "org.bluez.Error.InProgress") == 0 ||
             strcmp(error.name, "org.bluez.Error.AlreadyConnected") == 0);
        if (!in_progress) {
            /* A cached, unbonded device that failed to answer is stale (the
             * watch reset its address). Remove it and scan once more. Never
             * drop a bonded device on a plain timeout: the watch simply is not
             * advertising; dropping it would destroy a good bond. */
            bool paired = false;
            (void)get_bool_prop(bluez, "Paired", &paired);
            *remove_and_retry =
                auth_error(&error) || (!scan.fresh && !paired);
            set_error(bluez, "Connect", &error);
            sd_bus_error_free(&error);
            return WC_SESSION_ERR_TRANSPORT;
        }
    }
    sd_bus_error_free(&error);

    if (cancel_requested(bluez)) {
        copy_str(bluez->error, sizeof bluez->error, "cancelled");
        return WC_SESSION_ERR_TIMEOUT;
    }

    if (wait_services_resolved(bluez, 20000) < 0) {
        return WC_SESSION_ERR_TIMEOUT;
    }
    /* BlueZ can report ServicesResolved from a stale cache a moment before the
     * characteristic objects reappear. Retry the tree walk briefly. */
    int resolve_tries = 0;
    while (resolve_gatt(bluez) < 0 && resolve_tries++ < 20) {
        if (!pump_for(bluez, now_ms() + 200)) {
            copy_str(bluez->error, sizeof bluez->error,
                     cancel_requested(bluez) ? "cancelled"
                                             : "D-Bus error while resolving GATT");
            return WC_SESSION_ERR_TIMEOUT;
        }
    }
    if (bluez->request_path[0] == '\0' ||
        bluez->settings_path[0] == '\0') {
        return WC_SESSION_ERR_TRANSPORT;
    }

    if (bluez->props_slot == NULL) {
        sd_bus_slot *slot = NULL;
        r = sd_bus_add_match(
            bluez->bus, &slot,
            "type='signal',sender='org.bluez',"
            "interface='org.freedesktop.DBus.Properties',"
            "member='PropertiesChanged'",
            on_properties_changed, bluez);
        if (r < 0) {
            copy_str(bluez->error, sizeof bluez->error,
                     "cannot subscribe to GATT notifications");
            return WC_SESSION_ERR_TRANSPORT;
        }
        bluez->props_slot = slot;
    }

    if (start_notifications(bluez) < 0) {
        return WC_SESSION_ERR_TRANSPORT;
    }
    return WC_SESSION_OK;
}

int wc_bluez_connect(wc_bluez *bluez, const char *name, const char *address,
                     uint32_t scan_timeout_ms)
{
    if (bluez == NULL) {
        return WC_SESSION_ERR_INVALID;
    }
    clear_error(bluez);
    bluez->link_lost = false;

    bool remove_and_retry = false;
    int rc = connect_and_resolve(bluez, name, address, scan_timeout_ms,
                                 &remove_and_retry);
    if (rc == WC_SESSION_OK || !remove_and_retry ||
        cancel_requested(bluez)) {
        return rc;
    }

    /* The watch was reset or re-paired but BlueZ still holds the old key or
     * address: Connect fails before GATT. Drop the stale device once and scan
     * again. */
    char stale_path[PATH_MAX_];
    copy_str(stale_path, sizeof stale_path, bluez->device_path);
    if (remove_device_path(bluez, stale_path) < 0) {
        return WC_SESSION_ERR_TRANSPORT;
    }
    bluez->device_path[0] = '\0';
    bluez->address[0] = '\0';
    bluez->name[0] = '\0';
    bluez->head = 0;
    bluez->count = 0;
    reset_gatt(bluez);
    pump_for(bluez, now_ms() + 300);
    clear_error(bluez);

    bool retry_again = false;
    return connect_and_resolve(bluez, name, address, scan_timeout_ms,
                               &retry_again);
}

int wc_bluez_forget(wc_bluez *bluez, const char *address)
{
    if (bluez == NULL) {
        return WC_SESSION_ERR_INVALID;
    }
    clear_error(bluez);

    char path[PATH_MAX_];
    path[0] = '\0';
    if (address != NULL && *address != '\0') {
        scan_ctx scan;
        memset(&scan, 0, sizeof scan);
        scan.address = address;
        if (foreach_object(bluez, find_device_cb, &scan) < 0) {
            return WC_SESSION_ERR_TRANSPORT;
        }
        if (!scan.found) {
            copy_str(bluez->error, sizeof bluez->error,
                     "that watch is not in the BlueZ object tree");
            return WC_SESSION_ERR_TRANSPORT;
        }
        copy_str(path, sizeof path, scan.path);
    } else if (bluez->device_path[0] != '\0') {
        copy_str(path, sizeof path, bluez->device_path);
    } else {
        copy_str(bluez->error, sizeof bluez->error,
                 "no watch address given and no device connected");
        return WC_SESSION_ERR_INVALID;
    }

    if (remove_device_path(bluez, path) < 0) {
        return WC_SESSION_ERR_TRANSPORT;
    }
    bluez->device_path[0] = '\0';
    bluez->address[0] = '\0';
    bluez->name[0] = '\0';
    bluez->head = 0;
    bluez->count = 0;
    reset_gatt(bluez);
    clear_error(bluez);
    return WC_SESSION_OK;
}

const wc_transport *wc_bluez_transport(wc_bluez *bluez)
{
    return bluez != NULL ? &bluez->transport : NULL;
}

const char *wc_bluez_address(const wc_bluez *bluez)
{
    return bluez != NULL ? bluez->address : "";
}

const char *wc_bluez_name(const wc_bluez *bluez)
{
    return bluez != NULL ? bluez->name : "";
}

const char *wc_bluez_error(const wc_bluez *bluez)
{
    return bluez != NULL ? bluez->error : "no transport";
}

void wc_bluez_set_cancel(wc_bluez *bluez, wc_cancel_fn cancel, void *ctx)
{
    if (bluez == NULL) {
        return;
    }
    bluez->cancel = cancel;
    bluez->cancel_ctx = ctx;
}

int wc_bluez_pump(wc_bluez *bluez, uint32_t timeout_ms)
{
    if (bluez == NULL || bluez->bus == NULL) {
        return WC_SESSION_ERR_INVALID;
    }
    if (!pump_for(bluez, now_ms() + timeout_ms)) {
        return cancel_requested(bluez) ? WC_SESSION_ERR_TIMEOUT
                                       : WC_SESSION_ERR_TRANSPORT;
    }
    return WC_SESSION_OK;
}

bool wc_bluez_link_lost(const wc_bluez *bluez)
{
    return bluez != NULL && bluez->link_lost;
}

void wc_bluez_free(wc_bluez *bluez)
{
    if (bluez == NULL) {
        return;
    }
    if (bluez->bus != NULL) {
        bluez_close(bluez);
        unregister_agent(bluez);
        sd_bus_slot_unref(bluez->props_slot);
        sd_bus_flush_close_unref(bluez->bus);
        bluez->bus = NULL;
    }
    free(bluez);
}
