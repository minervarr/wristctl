#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/random.h>
#endif

#include "wristctl/bluez.h"
#include "wristctl/generated.h"
#include "wristctl/session.h"

typedef struct {
    const char *name;
    const char *address;
    const char *timezone;
    uint32_t wait_ms;
    int lead_ms;
    bool forget;
    bool init;
    bool read_after;
} cli_options;

static void usage(FILE *out)
{
    fprintf(out,
            "usage: wristctl-cli <command> [options]\n"
            "\n"
            "commands:\n"
            "  probe   scan, connect, pair and complete the handshake\n"
            "  read    handshake, then read settings, alarms and timer\n"
            "  sync    handshake, then write the current local time\n"
            "  forget  remove BlueZ's device object and bond\n"
            "\n"
            "options:\n"
            "  --name NAME        advertised name fragment (default: Casio prefixes)\n"
            "  --address ADDR     use a specific BLE address instead of scanning\n"
            "  --wait MS          scan window in milliseconds (default: 30000)\n"
            "  --timezone ZONE    IANA zone for the time write (default: process TZ)\n"
            "  --lead-ms MS       whole-second lead for sync (default: 25)\n"
            "  --init             send 00 01 all-features init before the time\n"
            "  --read             after probe's handshake, read settings/alarms/timer\n"
            "  --forget           remove the device before connecting\n"
            "  --help             this text\n");
}

static bool parse_uint(const char *text, unsigned long *out)
{
    char *end = NULL;
    if (text == NULL || *text == '\0') {
        return false;
    }
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

static int parse_options(int argc, char **argv, cli_options *options)
{
    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(stdout);
            exit(0);
        } else if (strcmp(arg, "--name") == 0 && i + 1 < argc) {
            options->name = argv[++i];
        } else if (strcmp(arg, "--address") == 0 && i + 1 < argc) {
            options->address = argv[++i];
        } else if (strcmp(arg, "--timezone") == 0 && i + 1 < argc) {
            options->timezone = argv[++i];
        } else if (strcmp(arg, "--wait") == 0 && i + 1 < argc) {
            unsigned long value = 0;
            if (!parse_uint(argv[++i], &value) || value == 0) {
                fprintf(stderr, "error: --wait needs a positive millisecond value\n");
                return -1;
            }
            options->wait_ms = (uint32_t)value;
        } else if (strcmp(arg, "--lead-ms") == 0 && i + 1 < argc) {
            unsigned long value = 0;
            if (!parse_uint(argv[++i], &value) || value > 500) {
                fprintf(stderr, "error: --lead-ms must be between 0 and 500\n");
                return -1;
            }
            options->lead_ms = (int)value;
        } else if (strcmp(arg, "--forget") == 0) {
            options->forget = true;
        } else if (strcmp(arg, "--init") == 0) {
            options->init = true;
        } else if (strcmp(arg, "--read") == 0) {
            options->read_after = true;
        } else {
            fprintf(stderr, "error: unknown option: %s\n", arg);
            return -1;
        }
    }
    return 0;
}

static void make_app_id(uint8_t id[WC_APP_ID_LEN])
{
#if defined(__linux__)
    if (getrandom(id, WC_APP_ID_LEN, 0) == (ssize_t)WC_APP_ID_LEN) {
        return;
    }
#endif
    uint32_t state = (uint32_t)time(NULL) ^ (uint32_t)getpid();
    for (size_t i = 0; i < WC_APP_ID_LEN; i++) {
        state = state * 1664525u + 1013904223u;
        id[i] = (uint8_t)(state >> 24);
    }
}

static wc_bluez *open_watch(const cli_options *options)
{
    wc_bluez *bluez = wc_bluez_new();
    if (bluez == NULL) {
        fprintf(stderr, "error: cannot open the system bus (is bluetoothd running?)\n");
        return NULL;
    }

    if (options->forget) {
        if (options->address == NULL || *options->address == '\0') {
            fprintf(stderr, "error: --forget needs --address\n");
            wc_bluez_free(bluez);
            return NULL;
        }
        printf("forgetting %s in BlueZ...\n", options->address);
        if (wc_bluez_forget(bluez, options->address) != WC_SESSION_OK) {
            fprintf(stderr, "forget failed: %s\n", wc_bluez_error(bluez));
            wc_bluez_free(bluez);
            return NULL;
        }
    }

    if (options->name != NULL) {
        printf("scanning for \"%s\" (%u ms)... press a button on the watch\n",
               options->name, options->wait_ms);
    } else {
        printf("scanning for the watch (%u ms)... press a button on the watch\n",
               options->wait_ms);
    }

    int rc = wc_bluez_connect(bluez, options->name, options->address,
                              options->wait_ms);
    if (rc != WC_SESSION_OK) {
        fprintf(stderr, "connect failed: %s\n", wc_bluez_error(bluez));
        wc_bluez_free(bluez);
        return NULL;
    }
    printf("connected: %s (%s)\n", wc_bluez_name(bluez),
           wc_bluez_address(bluez));
    return bluez;
}

static void make_local_time(wc_current_time *out)
{
    struct timespec now;
    struct tm local;
    clock_gettime(CLOCK_REALTIME, &now);
    localtime_r(&now.tv_sec, &local);
    memset(out, 0, sizeof *out);
    out->year = (uint16_t)(local.tm_year + 1900);
    out->month = (uint8_t)(local.tm_mon + 1);
    out->day = (uint8_t)local.tm_mday;
    out->hour = (uint8_t)local.tm_hour;
    out->minute = (uint8_t)local.tm_min;
    out->second = (uint8_t)local.tm_sec;
    out->weekday = (uint8_t)local.tm_wday;
    out->nano = (uint8_t)((now.tv_nsec * 256 / 1000000000) & 0xFF);
}

static wc_session *handshake(wc_bluez *bluez, wc_identity *identity,
                             const wc_current_time *time)
{
    uint8_t app_id[WC_APP_ID_LEN];
    make_app_id(app_id);

    wc_session *session = wc_session_new(wc_bluez_transport(bluez), app_id,
                                         5000);
    if (session == NULL) {
        fprintf(stderr, "error: cannot allocate the session\n");
        return NULL;
    }

    printf("handshake... (the watch sends 0x47 02 when it wants pairing)\n");
    int rc = wc_session_handshake_with_time(session, identity, time, true,
                                            5000);
    if (rc != WC_SESSION_OK) {
        wc_error error;
        if (wc_session_last_error(session, &error)) {
            fprintf(stderr,
                    "watch rejected class 0x%02x (status 0x%02x)%s%s\n",
                    error.class_id, error.status,
                    wc_bluez_error(bluez)[0] != '\0' ? "; " : "",
                    wc_bluez_error(bluez));
        } else if (rc == WC_SESSION_ERR_TIMEOUT) {
            fprintf(stderr, "handshake timed out%s%s\n",
                    wc_bluez_error(bluez)[0] != '\0' ? ": " : "",
                    wc_bluez_error(bluez));
        } else {
            fprintf(stderr, "handshake failed (%d): %s\n", rc,
                    wc_bluez_error(bluez));
        }
        wc_session_free(session);
        return NULL;
    }

    printf("watch: %s\n", identity->name);
    printf("model: %s\n",
           identity->model != NULL ? identity->model->name : "unknown");
    printf("pairing: %s\n", wc_pairing_name(identity->pairing));
    if (identity->has_condition) {
        printf("battery: %u  temperature: %u C  mtu: %u\n",
               identity->condition.battery, identity->condition.temperature,
               identity->condition.mtu);
    }
    if (identity->pairing == WC_PAIRING_BOND_FAILED) {
        fprintf(stderr, "bond failed%s%s\n",
                wc_bluez_error(bluez)[0] != '\0' ? ": " : "",
                wc_bluez_error(bluez));
        wc_session_free(session);
        return NULL;
    }
    return session;
}

static bool read_state(wc_session *session, wc_bluez *bluez);

static int cmd_probe(const cli_options *options)
{
    wc_bluez *bluez = open_watch(options);
    if (bluez == NULL) {
        return 1;
    }
    wc_identity identity;
    memset(&identity, 0, sizeof identity);
    wc_current_time now;
    make_local_time(&now);
    wc_session *session = handshake(bluez, &identity, &now);
    int rc = session != NULL ? 0 : 1;
    if (session != NULL) {
        if (options->read_after && !read_state(session, bluez)) {
            rc = 1;
        }
        wc_session_free(session);
    }
    wc_bluez_free(bluez);
    return rc;
}

static void print_basic_settings(const wc_basic_setting *settings)
{
    printf("settings: flags=0x%02x 24h=%u date_ddmm=%u language=%u "
           "tone_off=%u auto_light_off=%u\n",
           settings->flags, settings->time_format_24h,
           settings->date_format_ddmm, settings->language,
           settings->button_tone_off, settings->auto_light_off);
}

static void print_alarms(const wc_alarm_first *first,
                         const wc_alarm_rest *rest)
{
    printf("alarm 1: %02u:%02u %s\n", first->hour, first->minute,
           first->enabled ? "on" : "off");
    for (int i = 0; i < 4; i++) {
        printf("alarm %d: %02u:%02u %s\n", i + 2, rest->alarms[i].hour,
               rest->alarms[i].minute,
               rest->alarms[i].enabled ? "on" : "off");
    }
}

static void report_read_failure(const char *what, wc_session *session,
                                wc_bluez *bluez)
{
    wc_error error;
    if (wc_session_last_error(session, &error)) {
        fprintf(stderr, "%s failed: watch rejected class 0x%02x (status 0x%02x)\n",
                what, error.class_id, error.status);
    } else if (wc_bluez_error(bluez)[0] != '\0') {
        fprintf(stderr, "%s failed: %s\n", what, wc_bluez_error(bluez));
    } else {
        fprintf(stderr, "%s failed: no response from the watch\n", what);
    }
}

static void print_timer(const wc_program_timer *timer)
{
    printf("timer: repeat=%u", timer->repeat);
    for (size_t i = 0; i < 6; i++) {
        if (timer->workouts[i].minutes == 0 &&
            timer->workouts[i].seconds == 0) {
            continue;
        }
        printf("  %02u:%02u", timer->workouts[i].minutes,
               timer->workouts[i].seconds);
    }
    printf("\n");
}

static bool read_state(wc_session *session, wc_bluez *bluez)
{
    wc_basic_setting settings;
    wc_alarm_first first;
    wc_alarm_rest rest;
    wc_program_timer timer;
    bool ok = true;

    if (wc_session_read_basic_settings(session, &settings) == WC_SESSION_OK) {
        print_basic_settings(&settings);
    } else {
        report_read_failure("basic settings", session, bluez);
        ok = false;
    }
    if (wc_session_read_alarms(session, &first, &rest) == WC_SESSION_OK) {
        print_alarms(&first, &rest);
    } else {
        report_read_failure("alarms", session, bluez);
        ok = false;
    }
    if (wc_session_read_program_timer(session, &timer) == WC_SESSION_OK) {
        print_timer(&timer);
    } else {
        report_read_failure("program timer", session, bluez);
        ok = false;
    }
    return ok;
}

static int cmd_read(const cli_options *options)
{
    wc_bluez *bluez = open_watch(options);
    if (bluez == NULL) {
        return 1;
    }
    wc_identity identity;
    memset(&identity, 0, sizeof identity);
    wc_current_time now;
    make_local_time(&now);
    wc_session *session = handshake(bluez, &identity, &now);
    if (session == NULL) {
        wc_bluez_free(bluez);
        return 1;
    }

    bool ok = read_state(session, bluez);
    wc_session_free(session);
    wc_bluez_free(bluez);
    return ok ? 0 : 1;
}

static int cmd_sync(const cli_options *options)
{
    wc_bluez *bluez = open_watch(options);
    if (bluez == NULL) {
        return 1;
    }
    wc_identity identity;
    memset(&identity, 0, sizeof identity);
    /* Write the time before waiting for 0x47 02, exactly like probe: a fresh
     * watch only starts the bond after that write. The precise frame below
     * follows on the same session. */
    wc_current_time handshake_time;
    make_local_time(&handshake_time);
    wc_session *session = handshake(bluez, &identity, &handshake_time);
    if (session == NULL) {
        wc_bluez_free(bluez);
        return 1;
    }

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    int64_t target_ms = (now_ms / 1000 + 1) * 1000;
    if (target_ms - now_ms < options->lead_ms + 250) {
        target_ms += 1000;
    }
    time_t target = (time_t)(target_ms / 1000);
    struct tm local;
    if (localtime_r(&target, &local) == NULL) {
        fprintf(stderr, "error: localtime failed\n");
        wc_session_free(session);
        wc_bluez_free(bluez);
        return 1;
    }

    wc_current_time value;
    memset(&value, 0, sizeof value);
    value.year = (uint16_t)(local.tm_year + 1900);
    value.month = (uint8_t)(local.tm_mon + 1);
    value.day = (uint8_t)local.tm_mday;
    value.hour = (uint8_t)local.tm_hour;
    value.minute = (uint8_t)local.tm_min;
    value.second = (uint8_t)local.tm_sec;
    value.weekday = (uint8_t)local.tm_wday;

    uint8_t frame[WC_LEN_CURRENT_TIME];
    size_t length = 0;
    if (wc_current_time_encode(&value, frame, sizeof frame, &length) != 0) {
        fprintf(stderr, "error: cannot encode the time frame\n");
        wc_session_free(session);
        wc_bluez_free(bluez);
        return 1;
    }

    printf("target: %04d-%02u-%02u %02u:%02u:%02u %s\n", value.year,
           value.month, value.day, value.hour, value.minute, value.second,
           options->timezone != NULL ? options->timezone
                                     : (getenv("TZ") != NULL ? getenv("TZ")
                                                             : "system TZ"));
    printf("frame:");
    for (size_t i = 0; i < length; i++) {
        printf(" %02x", frame[i]);
    }
    printf("\n");

    int rc = WC_SESSION_OK;
    if (options->init) {
        /* The vendor app sends this two-byte "all features init" to the
         * all-features characteristic (26eb002d) with the first time write.
         * The watch does not acknowledge it, so use write-without-response. */
        static const uint8_t features_init[] = {0x00, 0x01};
        rc = wc_session_write_all_no_response(session, features_init,
                                              sizeof features_init);
        if (rc != WC_SESSION_OK) {
            fprintf(stderr, "all-features init failed: %s\n",
                    wc_bluez_error(bluez));
            wc_session_free(session);
            wc_bluez_free(bluez);
            return 1;
        }
        printf("all-features init sent\n");
    }

    rc = wc_session_write_all(session, frame, length);
    if (rc != WC_SESSION_OK) {
        fprintf(stderr, "time write failed: %s\n", wc_bluez_error(bluez));
        wc_session_free(session);
        wc_bluez_free(bluez);
        return 1;
    }
    printf("time written%s (0x09 has no readback; check the watch display)\n",
           options->init ? " and all-features init sent" : "");
    wc_session_free(session);
    wc_bluez_free(bluez);
    return 0;
}

static int cmd_forget(const cli_options *options)
{
    if (options->address == NULL || *options->address == '\0') {
        fprintf(stderr, "error: forget needs --address\n");
        return 1;
    }
    wc_bluez *bluez = wc_bluez_new();
    if (bluez == NULL) {
        fprintf(stderr, "error: cannot open the system bus\n");
        return 1;
    }
    int rc = wc_bluez_forget(bluez, options->address);
    if (rc != WC_SESSION_OK) {
        fprintf(stderr, "forget failed: %s\n", wc_bluez_error(bluez));
    } else {
        printf("removed %s from BlueZ (bond and cached GATT)\n",
               options->address);
    }
    wc_bluez_free(bluez);
    return rc == WC_SESSION_OK ? 0 : 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(stdout);
        return 0;
    }

    cli_options options;
    memset(&options, 0, sizeof options);
    options.wait_ms = 30000;
    options.lead_ms = 25;

    if (parse_options(argc, argv, &options) != 0) {
        return 2;
    }

    if (options.timezone != NULL && *options.timezone != '\0') {
        if (setenv("TZ", options.timezone, 1) != 0) {
            fprintf(stderr, "error: cannot set TZ=%s\n", options.timezone);
            return 1;
        }
        tzset();
    }

    if (strcmp(argv[1], "probe") == 0) {
        return cmd_probe(&options);
    }
    if (strcmp(argv[1], "read") == 0) {
        return cmd_read(&options);
    }
    if (strcmp(argv[1], "sync") == 0) {
        return cmd_sync(&options);
    }
    if (strcmp(argv[1], "forget") == 0) {
        return cmd_forget(&options);
    }

    fprintf(stderr, "error: unknown command: %s\n", argv[1]);
    usage(stderr);
    return 2;
}
