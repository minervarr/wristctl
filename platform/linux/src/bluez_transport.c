#define _POSIX_C_SOURCE 200809L

#include "wristctl/bluez.h"

#include <ctype.h>
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
        } else if (sd_bus_message_skip(msg, "v") < 0) {
            return -1;
        }
        if (sd_bus_message_exit_container(msg) < 0) {
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
} scan_ctx;

static void scan_take_device(scan_ctx *scan, const char *path,
                             const obj_props *props)
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
    scan_take_device(scan, path, props);
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
            scan_take_device(scan, path, &props);
        }
        sd_bus_message_exit_container(msg);
    }
    sd_bus_message_exit_container(msg);
    return 0;
}

static void pump_for(wc_bluez *bluez, uint64_t deadline)
{
    for (;;) {
        int r = sd_bus_process(bluez->bus, NULL);
        if (r < 0) {
            return;
        }
        if (r > 0) {
            continue;
        }
        uint64_t now = now_ms();
        if (now >= deadline) {
            return;
        }
        if (sd_bus_wait(bluez->bus, (deadline - now) * 1000u) < 0) {
            return;
        }
    }
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

    sd_bus_error error = SD_BUS_ERROR_NULL;
    r = sd_bus_call_method(bluez->bus, BLUEZ_DEST, bluez->adapter_path,
                           ADAPTER_IFACE, "StartDiscovery", &error, NULL, "");
    if (r < 0) {
        /* A discovery already in progress is not fatal. */
        clear_error(bluez);
    }
    sd_bus_error_free(&error);

    uint64_t deadline = now_ms() + (timeout_ms != 0 ? timeout_ms : 12000);
    while (!scan->found && now_ms() < deadline) {
        sd_bus_process(bluez->bus, NULL);
        uint64_t now = now_ms();
        if (now >= deadline) {
            break;
        }
        sd_bus_wait(bluez->bus, (deadline - now) * 1000u);
    }

    error = SD_BUS_ERROR_NULL;
    sd_bus_call_method(bluez->bus, BLUEZ_DEST, bluez->adapter_path,
                       ADAPTER_IFACE, "StopDiscovery", &error, NULL, "");
    sd_bus_error_free(&error);
    sd_bus_slot_unref(slot);

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
        pump_for(bluez, now_ms() + 100);
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
        gatt->chars[i].notify =
            strcmp(props->uuid, SETTINGS_UUID) == 0 ||
            strcmp(props->uuid, NOTIFY_UUID) == 0;
    }
}

static int resolve_gatt(wc_bluez *bluez)
{
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
                    ring_push(bluez, data, size);
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
    return write_char(bluez, path, data, len, type);
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

static int bluez_pair(void *ctx, const char *address)
{
    (void)address;
    wc_bluez *bluez = ctx;
    if (bluez->device_path[0] == '\0') {
        return 0;
    }
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(bluez->bus, BLUEZ_DEST, bluez->device_path,
                               DEVICE_IFACE, "Pair", &error, NULL, "");
    sd_bus_error_free(&error);
    return r >= 0 ? 1 : 0;
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
    bluez->transport.address = bluez->address;
    return bluez;
}

int wc_bluez_connect(wc_bluez *bluez, const char *name, const char *address,
                     uint32_t scan_timeout_ms)
{
    if (bluez == NULL) {
        return WC_SESSION_ERR_INVALID;
    }
    clear_error(bluez);

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
            set_error(bluez, "Connect", &error);
            sd_bus_error_free(&error);
            return WC_SESSION_ERR_TRANSPORT;
        }
    }
    sd_bus_error_free(&error);

    if (wait_services_resolved(bluez, 15000) < 0) {
        return WC_SESSION_ERR_TIMEOUT;
    }
    if (resolve_gatt(bluez) < 0) {
        return WC_SESSION_ERR_TRANSPORT;
    }

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

    for (size_t i = 0; i < bluez->notify_count; i++) {
        error = SD_BUS_ERROR_NULL;
        r = sd_bus_call_method(bluez->bus, BLUEZ_DEST, bluez->notify_paths[i],
                               GATT_CHAR_IFACE, "StartNotify", &error, NULL,
                               "");
        if (r < 0) {
            set_error(bluez, "StartNotify", &error);
            sd_bus_error_free(&error);
            return WC_SESSION_ERR_TRANSPORT;
        }
        sd_bus_error_free(&error);
    }
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

void wc_bluez_free(wc_bluez *bluez)
{
    if (bluez == NULL) {
        return;
    }
    if (bluez->bus != NULL) {
        bluez_close(bluez);
        sd_bus_slot_unref(bluez->props_slot);
        sd_bus_flush_close_unref(bluez->bus);
        bluez->bus = NULL;
    }
    free(bluez);
}
