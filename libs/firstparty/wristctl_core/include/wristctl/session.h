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
    WC_WRITE_NOTIFICATION = 2
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

typedef struct wc_transport {
    void *ctx;
    wc_write_fn write;
    wc_read_fn read;
    wc_lla_fn write_link_loss_alert; /* optional */
    wc_pair_fn pair;                 /* optional */
    wc_close_fn close;               /* optional */
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

int wc_session_read_basic_settings(wc_session *session, wc_basic_setting *out);
int wc_session_read_alarms(wc_session *session, wc_alarm_first *first,
                           wc_alarm_rest *rest);
int wc_session_read_program_timer(wc_session *session, wc_program_timer *out);

/* Details of the last WC_SESSION_ERR_REJECTED. */
bool wc_session_last_error(const wc_session *session, wc_error *out);

const char *wc_pairing_name(wc_pairing pairing);

#ifdef __cplusplus
}
#endif

#endif /* WRISTCTL_SESSION_H */
