/* Transport-agnostic watch session.
 *
 * The session owns request/response correlation, the connection handshake and
 * the parked-frame queue. It never touches a socket, a BLE stack or a clock:
 * the frontend supplies the transport callbacks and blocks inside them.
 *
 * Order of operations follows the observed vendor session: identify,
 * configure, then answer the watch's service-discovery request (0x47 02) with
 * either a bond or a Link Loss Alert write, and wait for 0x47 01.
 *
 * One session is single-threaded. A GUI runs it on a worker thread and posts
 * results back; tests drive it synchronously.
 */
#ifndef WRISTCTL_SESSION_H
#define WRISTCTL_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wristctl/generated.h"
#include "wristctl/models.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The longest frame in spec/classes.json is 20 bytes; leave room for a
 * transport that coalesces or a future class. */
#define WC_FRAME_MAX 32
#define WC_APP_ID_LEN 10
#define WC_SESSION_MAX_PARKED 64
#define WC_NAME_MAX 20

typedef struct {
    uint8_t len;
    uint8_t data[WC_FRAME_MAX];
} wc_frame;

typedef enum {
    WC_WRITE_REQUEST = 0,  /* request characteristic, write without response */
    WC_WRITE_ALL = 1,      /* settings characteristic, write with response */
    WC_WRITE_NOTIFICATION = 2,
    WC_WRITE_ALL_COMMAND = 3 /* settings characteristic, write without response */
} wc_write_kind;

/* Return conventions:
 *   write      0 on success, negative on failure
 *   read       frame length > 0, 0 on timeout, negative on failure
 *   lla        0 on success, negative when the characteristic is absent
 *   pair       1 when bonded, 0 otherwise
 *   close      void
 */
typedef int (*wc_write_fn)(void *ctx, wc_write_kind kind,
                           const uint8_t *data, size_t len);
typedef int (*wc_read_fn)(void *ctx, uint8_t *buf, size_t cap,
                          uint32_t timeout_ms);
typedef int (*wc_lla_fn)(void *ctx, uint8_t level);
typedef int (*wc_pair_fn)(void *ctx, const char *address);
typedef void (*wc_close_fn)(void *ctx);
typedef uint64_t (*wc_now_ms_fn)(void *ctx);

typedef struct wc_transport {
    void *ctx;
    wc_write_fn write;
    wc_read_fn read;
    wc_lla_fn write_link_loss_alert; /* optional */
    wc_pair_fn pair;                 /* optional */
    wc_close_fn close;               /* optional */
    /* Optional monotonic clock, supplied by the frontend. When set, a wait
     * keeps one deadline across unrelated parked frames instead of restarting
     * the timeout for every frame. */
    wc_now_ms_fn now_ms;
    const char *address;             /* optional, must outlive the session */
} wc_transport;

typedef enum {
    WC_SESSION_OK = 0,
    WC_SESSION_ERR_INVALID = -1,
    WC_SESSION_ERR_NOMEM = -2,
    WC_SESSION_ERR_TIMEOUT = -3,
    WC_SESSION_ERR_TRANSPORT = -4,
    WC_SESSION_ERR_REJECTED = -5
} wc_status;

typedef enum {
    WC_PAIRING_UNKNOWN = 0,
    WC_PAIRING_NOT_OBSERVED,
    WC_PAIRING_BOND_REQUESTED,
    WC_PAIRING_BOND_FAILED,
    WC_PAIRING_LINK_LOSS_ALERT,
    WC_PAIRING_LINK_LOSS_ALERT_UNAVAILABLE,
    WC_PAIRING_UNEXPECTED_COMMAND,
    WC_PAIRING_UNEXPECTED_DONE,
    WC_PAIRING_ERROR
} wc_pairing;

typedef struct {
    char name[WC_NAME_MAX];
    const wc_model *model;
    char address[64];
    wc_frame ble_features;
    wc_frame ble_settings;
    wc_frame advertise_parameters;
    wc_frame connection_parameters;
    wc_frame module;
    wc_frame version;
    wc_frame condition_raw;
    wc_watch_condition condition;
    bool has_condition;
    wc_pairing pairing;
} wc_identity;

typedef struct wc_session wc_session;

/* Copies the transport struct (not the address string). `app_id` must be
 * WC_APP_ID_LEN bytes; `timeout_ms` applies to each request. Returns NULL on
 * invalid arguments or allocation failure. */
wc_session *wc_session_new(const wc_transport *transport,
                           const uint8_t app_id[WC_APP_ID_LEN],
                           uint32_t timeout_ms);
void wc_session_free(wc_session *session);

/* Full handshake. discovery_timeout_ms is the wait for the watch's 0x47 02;
 * a timeout is reported as WC_PAIRING_NOT_OBSERVED, not an error. */
int wc_session_handshake(wc_session *session, wc_identity *out,
                         bool expect_service_discovery,
                         uint32_t discovery_timeout_ms);

/* Same handshake, but writes `time` (0x09) after the version read and before
 * waiting for the watch's 0x47 02. The vendor app does this on every
 * connection; the recorded GBX-100 pairing only starts after that write.
 * `time` may be NULL, which is exactly wc_session_handshake(). */
int wc_session_handshake_with_time(wc_session *session, wc_identity *out,
                                   const wc_current_time *time,
                                   bool expect_service_discovery,
                                   uint32_t discovery_timeout_ms);

/* Generic request/response. `request` writes one class byte, `request_bytes`
 * writes the whole frame. `out` receives the matching frame. */
int wc_session_request(wc_session *session, uint8_t class_id,
                       wc_frame *out, uint32_t timeout_ms);
int wc_session_request_bytes(wc_session *session, const uint8_t *payload,
                             size_t len, wc_frame *out, uint32_t timeout_ms);

/* Return the next frame of `class_id`, parking unrelated frames. An error
 * class (0xFF) is recorded and returned as WC_SESSION_ERR_REJECTED. */
int wc_session_wait_for(wc_session *session, uint8_t class_id,
                        wc_frame *out, uint32_t timeout_ms);

/* Write with response to the settings characteristic. */
int wc_session_write_all(wc_session *session, const uint8_t *data, size_t len);

/* Write to the settings characteristic without waiting for an ATT response.
 * The watch does not acknowledge the two-byte 00 01 init frame. */
int wc_session_write_all_no_response(wc_session *session, const uint8_t *data,
                                     size_t len);

int wc_session_read_basic_settings(wc_session *session, wc_basic_setting *out);
int wc_session_read_alarms(wc_session *session, wc_alarm_first *first,
                           wc_alarm_rest *rest);
int wc_session_read_program_timer(wc_session *session, wc_program_timer *out);
int wc_session_read_dst_watch_state(wc_session *session,
                                    wc_dst_watch_state *out);
/* The watch advances to the next unit after each 0x1E read. The returned
 * structure identifies the unit; callers must not assume a requested unit. */
int wc_session_read_dst_setting(wc_session *session, wc_dst_setting *out);
int wc_session_read_city_name(wc_session *session, uint8_t unit,
                              wc_city_name *out);

/* Write paths. Each writes the settings characteristic (write with response)
 * and returns WC_SESSION_OK once the ATT write is acknowledged. The watch does
 * not send a read-back frame for these classes; a caller that wants proof must
 * read the class again.
 *
 * Alarms are two frames: 0x15 for alarm 1 and 0x16 for alarms 2-5. The watch
 * expects both when reading; the vendor app writes the same pair. `constant`
 * is preserved from the read value by the caller, including the reset watch's
 * zero.
 *
 * The program timer write replaces all six entries and the repeat count, so a
 * frontend must read the current value and confirm the replacement first. */
int wc_session_write_alarms(wc_session *session, const wc_alarm_first *first,
                            const wc_alarm_rest *rest);
int wc_session_write_basic_settings(wc_session *session,
                                    const wc_basic_setting *settings);
int wc_session_write_program_timer(wc_session *session,
                                   const wc_program_timer *timer);
/* `name` is UTF-8 and at most 19 bytes. Shorter names are NUL padded. */
int wc_session_write_watch_name(wc_session *session, const char *name);
int wc_session_write_dst_watch_state(wc_session *session,
                                     const wc_dst_watch_state *state);
int wc_session_write_dst_setting(wc_session *session,
                                 const wc_dst_setting *setting);
int wc_session_write_city_name(wc_session *session, const wc_city_name *name);

/* App notification on 26eb0030-... (write without response). `payload` is the
 * already encoded, XOR-obfuscated 0x30 packet (see wristctl/notify.h). */
int wc_session_send_notification(wc_session *session, const uint8_t *payload,
                                 size_t len);

/* Details of the last WC_SESSION_ERR_REJECTED. */
bool wc_session_last_error(const wc_session *session, wc_error *out);

const char *wc_pairing_name(wc_pairing pairing);

#ifdef __cplusplus
}
#endif

#endif /* WRISTCTL_SESSION_H */
