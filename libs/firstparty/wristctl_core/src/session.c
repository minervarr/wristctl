#include "wristctl/session.h"

#include <stdlib.h>
#include <string.h>
#define CLASS_WATCH_NAME 0x23
#define CLASS_BLE_FEATURES 0x10
#define CLASS_BLE_SETTING 0x11
#define CLASS_ADVERTISE 0x3B
#define CLASS_CONNECTION 0x3A
#define CLASS_MODULE 0x26
#define CLASS_CONDITION 0x28
#define CLASS_VERSION 0x20
#define CLASS_SERVICE_DISCOVERY 0x47
#define CLASS_ERROR 0xFF

#define SERVICE_DISCOVERY_START 2
#define SERVICE_DISCOVERY_DONE 1

#define CLASS_BASIC_SETTING 0x13
#define CLASS_ALARM_FIRST 0x15
#define CLASS_ALARM_REST 0x16
#define CLASS_PROGRAM_TIMER 0x2A

typedef struct {
    uint8_t class_id;
    wc_frame frame;
} wc_parked;

struct wc_session {
    wc_transport transport;
    uint8_t app_id[WC_APP_ID_LEN];
    uint32_t timeout_ms;
    wc_parked parked[WC_SESSION_MAX_PARKED];
    size_t parked_count;
    const wc_model *model;
    wc_error last_error;
    bool has_error;
};

static void frame_copy(wc_frame *dst, const uint8_t *src, size_t len)
{
    if (len > WC_FRAME_MAX) {
        len = WC_FRAME_MAX;
    }
    dst->len = (uint8_t)len;
    if (len > 0) {
        memcpy(dst->data, src, len);
    }
}

static void copy_cstr(char *dst, size_t cap, const char *src)
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

static void park(wc_session *session, const wc_frame *frame)
{
    if (frame->len == 0) {
        return;
    }
    if (session->parked_count == WC_SESSION_MAX_PARKED) {
        memmove(&session->parked[0], &session->parked[1],
                sizeof(session->parked[0]) * (WC_SESSION_MAX_PARKED - 1));
        session->parked_count--;
    }
    wc_parked *slot = &session->parked[session->parked_count++];
    slot->class_id = frame->data[0];
    slot->frame = *frame;
}

static bool take_parked(wc_session *session, uint8_t class_id, wc_frame *out)
{
    for (size_t i = 0; i < session->parked_count; i++) {
        if (session->parked[i].class_id != class_id) {
            continue;
        }
        *out = session->parked[i].frame;
        memmove(&session->parked[i], &session->parked[i + 1],
                sizeof(session->parked[0]) * (session->parked_count - i - 1));
        session->parked_count--;
        return true;
    }
    return false;
}

static int read_next(wc_session *session, wc_frame *out, uint32_t timeout_ms)
{
    uint8_t buf[WC_FRAME_MAX];
    int n = session->transport.read(session->transport.ctx, buf, sizeof buf,
                                    timeout_ms);
    if (n < 0) {
        return WC_SESSION_ERR_TRANSPORT;
    }
    if (n == 0) {
        return WC_SESSION_ERR_TIMEOUT;
    }
    if ((size_t)n > sizeof buf) {
        return WC_SESSION_ERR_TRANSPORT;
    }
    frame_copy(out, buf, (size_t)n);
    return WC_SESSION_OK;
}

wc_session *wc_session_new(const wc_transport *transport,
                           const uint8_t app_id[WC_APP_ID_LEN],
                           uint32_t timeout_ms)
{
    if (transport == NULL || transport->write == NULL ||
        transport->read == NULL || app_id == NULL) {
        return NULL;
    }
    wc_session *session = calloc(1, sizeof *session);
    if (session == NULL) {
        return NULL;
    }
    session->transport = *transport;
    memcpy(session->app_id, app_id, WC_APP_ID_LEN);
    session->timeout_ms = timeout_ms != 0 ? timeout_ms : 5000;
    return session;
}

void wc_session_free(wc_session *session)
{
    if (session == NULL) {
        return;
    }
    if (session->transport.close != NULL) {
        session->transport.close(session->transport.ctx);
    }
    free(session);
}

int wc_session_wait_for(wc_session *session, uint8_t class_id, wc_frame *out,
                        uint32_t timeout_ms)
{
    if (session == NULL || out == NULL) {
        return WC_SESSION_ERR_INVALID;
    }
    if (take_parked(session, class_id, out)) {
        return WC_SESSION_OK;
    }

    for (;;) {
        wc_frame frame;
        int rc = read_next(session, &frame, timeout_ms);
        if (rc != WC_SESSION_OK) {
            return rc;
        }
        if (frame.len == 0) {
            continue;
        }
        if (frame.data[0] == class_id) {
            *out = frame;
            return WC_SESSION_OK;
        }
        if (frame.data[0] == CLASS_ERROR) {
            wc_error error;
            if (wc_error_decode(frame.data, frame.len, &error) == 0) {
                session->last_error = error;
                session->has_error = true;
            }
            return WC_SESSION_ERR_REJECTED;
        }
        park(session, &frame);
    }
}

int wc_session_request(wc_session *session, uint8_t class_id, wc_frame *out,
                       uint32_t timeout_ms)
{
    if (session == NULL) {
        return WC_SESSION_ERR_INVALID;
    }
    uint8_t request = class_id;
    if (session->transport.write(session->transport.ctx, WC_WRITE_REQUEST,
                                 &request, 1) < 0) {
        return WC_SESSION_ERR_TRANSPORT;
    }
    return wc_session_wait_for(session, class_id, out,
                               timeout_ms != 0 ? timeout_ms
                                               : session->timeout_ms);
}

int wc_session_request_bytes(wc_session *session, const uint8_t *payload,
                             size_t len, wc_frame *out, uint32_t timeout_ms)
{
    if (session == NULL || payload == NULL || len == 0) {
        return WC_SESSION_ERR_INVALID;
    }
    if (session->transport.write(session->transport.ctx, WC_WRITE_REQUEST,
                                 payload, len) < 0) {
        return WC_SESSION_ERR_TRANSPORT;
    }
    return wc_session_wait_for(session, payload[0], out,
                               timeout_ms != 0 ? timeout_ms
                                               : session->timeout_ms);
}

int wc_session_write_all(wc_session *session, const uint8_t *data, size_t len)
{
    if (session == NULL || data == NULL || len == 0) {
        return WC_SESSION_ERR_INVALID;
    }
    if (session->transport.write(session->transport.ctx, WC_WRITE_ALL, data,
                                 len) < 0) {
        return WC_SESSION_ERR_TRANSPORT;
    }
    return WC_SESSION_OK;
}

static int request_frame(wc_session *session, uint8_t class_id, wc_frame *out)
{
    return wc_session_request(session, class_id, out, session->timeout_ms);
}

static int store_frame(wc_frame *dst, const wc_frame *src)
{
    if (src->len > WC_FRAME_MAX) {
        return WC_SESSION_ERR_TRANSPORT;
    }
    *dst = *src;
    return WC_SESSION_OK;
}

int wc_session_handshake(wc_session *session, wc_identity *out,
                         bool expect_service_discovery,
                         uint32_t discovery_timeout_ms)
{
    if (session == NULL || out == NULL) {
        return WC_SESSION_ERR_INVALID;
    }
    memset(out, 0, sizeof *out);
    copy_cstr(out->address, sizeof out->address, session->transport.address);

    wc_frame frame;
    int rc = request_frame(session, CLASS_WATCH_NAME, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }
    wc_watch_name name;
    if (wc_watch_name_decode(frame.data, frame.len, &name) != 0) {
        return WC_SESSION_ERR_INVALID;
    }
    copy_cstr(out->name, sizeof out->name, name.name);
    out->model = wc_model_lookup(out->name);
    session->model = out->model;

    rc = request_frame(session, CLASS_BLE_FEATURES, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }
    rc = store_frame(&out->ble_features, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }

    wc_app_info info;
    memset(&info, 0, sizeof info);
    memcpy(info.app_id, session->app_id, WC_APP_ID_LEN);
    info.os = 2;
    uint8_t app_info[WC_LEN_APP_INFO];
    size_t app_info_len = 0;
    if (wc_app_info_encode(&info, app_info, sizeof app_info, &app_info_len) != 0) {
        return WC_SESSION_ERR_INVALID;
    }
    rc = wc_session_write_all(session, app_info, app_info_len);
    if (rc != WC_SESSION_OK) {
        return rc;
    }

    rc = request_frame(session, CLASS_BLE_SETTING, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }
    rc = store_frame(&out->ble_settings, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }

    rc = request_frame(session, CLASS_ADVERTISE, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }
    rc = store_frame(&out->advertise_parameters, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }

    rc = request_frame(session, CLASS_CONNECTION, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }
    rc = store_frame(&out->connection_parameters, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }

    rc = request_frame(session, CLASS_MODULE, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }
    rc = store_frame(&out->module, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }

    rc = request_frame(session, CLASS_CONDITION, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }
    rc = store_frame(&out->condition_raw, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }
    if (wc_watch_condition_decode(frame.data, frame.len, &out->condition) == 0) {
        out->has_condition = true;
    }

    rc = request_frame(session, CLASS_VERSION, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }
    rc = store_frame(&out->version, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }

    if (!expect_service_discovery) {
        return WC_SESSION_OK;
    }

    rc = wc_session_wait_for(session, CLASS_SERVICE_DISCOVERY, &frame,
                             discovery_timeout_ms);
    if (rc == WC_SESSION_ERR_TIMEOUT) {
        out->pairing = WC_PAIRING_NOT_OBSERVED;
        return WC_SESSION_OK;
    }
    if (rc != WC_SESSION_OK) {
        return rc;
    }

    uint8_t command = frame.len > 1 ? frame.data[1] : 0;
    if (command != SERVICE_DISCOVERY_START) {
        out->pairing = WC_PAIRING_UNEXPECTED_COMMAND;
        return WC_SESSION_OK;
    }

    const wc_model *model = session->model;
    if (model != NULL &&
        wc_model_flag(model, "isPairingInsteadOfLlsAl") == 1) {
        if (session->transport.pair != NULL) {
            (void)session->transport.pair(session->transport.ctx,
                                          out->address);
        }
        out->pairing = WC_PAIRING_BOND_REQUESTED;
    } else if (session->transport.write_link_loss_alert != NULL &&
               session->transport.write_link_loss_alert(
                   session->transport.ctx, 1) == 0) {
        out->pairing = WC_PAIRING_LINK_LOSS_ALERT;
    } else {
        out->pairing = WC_PAIRING_LINK_LOSS_ALERT_UNAVAILABLE;
    }

    rc = wc_session_wait_for(session, CLASS_SERVICE_DISCOVERY, &frame,
                             discovery_timeout_ms);
    if (rc != WC_SESSION_OK) {
        return rc;
    }
    if (frame.len > 1 && frame.data[1] != SERVICE_DISCOVERY_DONE) {
        out->pairing = WC_PAIRING_UNEXPECTED_DONE;
    }
    return WC_SESSION_OK;
}

int wc_session_read_basic_settings(wc_session *session, wc_basic_setting *out)
{
    if (session == NULL || out == NULL) {
        return WC_SESSION_ERR_INVALID;
    }
    wc_frame frame;
    int rc = request_frame(session, CLASS_BASIC_SETTING, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }
    return wc_basic_setting_decode(frame.data, frame.len, out) == 0
               ? WC_SESSION_OK
               : WC_SESSION_ERR_INVALID;
}

int wc_session_read_alarms(wc_session *session, wc_alarm_first *first,
                           wc_alarm_rest *rest)
{
    if (session == NULL || first == NULL || rest == NULL) {
        return WC_SESSION_ERR_INVALID;
    }
    wc_frame frame;
    int rc = request_frame(session, CLASS_ALARM_FIRST, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }
    if (wc_alarm_first_decode(frame.data, frame.len, first) != 0) {
        return WC_SESSION_ERR_INVALID;
    }
    rc = request_frame(session, CLASS_ALARM_REST, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }
    return wc_alarm_rest_decode(frame.data, frame.len, rest) == 0
               ? WC_SESSION_OK
               : WC_SESSION_ERR_INVALID;
}

int wc_session_read_program_timer(wc_session *session, wc_program_timer *out)
{
    if (session == NULL || out == NULL) {
        return WC_SESSION_ERR_INVALID;
    }
    wc_frame frame;
    int rc = request_frame(session, CLASS_PROGRAM_TIMER, &frame);
    if (rc != WC_SESSION_OK) {
        return rc;
    }
    return wc_program_timer_decode(frame.data, frame.len, out) == 0
               ? WC_SESSION_OK
               : WC_SESSION_ERR_INVALID;
}

bool wc_session_last_error(const wc_session *session, wc_error *out)
{
    if (session == NULL || out == NULL || !session->has_error) {
        return false;
    }
    *out = session->last_error;
    return true;
}

const char *wc_pairing_name(wc_pairing pairing)
{
    switch (pairing) {
    case WC_PAIRING_NOT_OBSERVED:
        return "not_observed";
    case WC_PAIRING_BOND_REQUESTED:
        return "bond_requested";
    case WC_PAIRING_LINK_LOSS_ALERT:
        return "link_loss_alert";
    case WC_PAIRING_LINK_LOSS_ALERT_UNAVAILABLE:
        return "link_loss_alert_unavailable";
    case WC_PAIRING_UNEXPECTED_COMMAND:
        return "unexpected_command";
    case WC_PAIRING_UNEXPECTED_DONE:
        return "unexpected_done";
    case WC_PAIRING_ERROR:
        return "error";
    case WC_PAIRING_UNKNOWN:
    default:
        return "unknown";
    }
}
