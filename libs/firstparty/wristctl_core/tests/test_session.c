#undef NDEBUG

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "wristctl/session.h"

/* Frames from the recorded GBX-100 session (work/evidence/att_frames_current.txt,
 * also pinned by py/tests/test_session.py). */
static const uint8_t NAME[] = {0x23, 0x43, 0x41, 0x53, 0x49, 0x4f, 0x20, 0x47,
                               0x42, 0x58, 0x2d, 0x31, 0x30, 0x30, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00};
static const uint8_t FEATURES[] = {0x10, 0x51, 0x46, 0x21, 0x44, 0x02, 0xda,
                                   0x7f, 0x09, 0x03, 0x0f, 0xff, 0xff, 0xff,
                                   0xff, 0x27, 0x00, 0x00, 0x00, 0x00};
static const uint8_t BLE_SETTINGS[] = {0x11, 0x00, 0x00, 0x00, 0x06, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
                                       0xff, 0xff, 0x00};
static const uint8_t ADVERTISE[] = {0x3b, 0x50, 0x00, 0x0a, 0x40, 0x01, 0x05,
                                    0x05, 0x40, 0x06, 0x0a, 0x02, 0xa0, 0x00,
                                    0x05, 0x40, 0x01, 0x05, 0x1c, 0x00};
static const uint8_t CONNECTION[] = {0x3a, 0x00, 0x34, 0x01, 0x40,
                                     0x01, 0x04, 0x00, 0x58, 0x02};
static const uint8_t MODULE[] = {0x26, 0xe4, 0x54, 0xe8, 0x41, 0x1e, 0x3b, 0x12,
                                 0x2f, 0xe7, 0x96, 0x00, 0x00, 0x01, 0xea,
                                 0xff, 0xff};
static const uint8_t CONDITION[] = {0x28, 0x06, 0x1e, 0x23,
                                    0x00, 0xba, 0x01, 0x00, 0x01};
static const uint8_t VERSION[] = {0x20, 0x15, 0x05, 0x00, 0x06, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x17,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t SD_START[] = {0x47, 0x02};
static const uint8_t SD_DONE[] = {0x47, 0x01};
static const uint8_t ERROR[] = {0xff, 0x81, 0x09};
static const uint8_t UNKNOWN_NAME[] = {0x23, 0x41, 0x43, 0x4d, 0x45, 0x20,
                                       0x57, 0x41, 0x54, 0x43, 0x48, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00};
static const uint8_t BASIC_SETTING[] = {0x13, 0x44, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
static const uint8_t PROGRAM_TIMER[] = {0x2a, 0x01, 0x00, 0x25, 0x00, 0x00,
                                        0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00};

typedef struct {
    const uint8_t *data;
    size_t len;
} frame_ref;

#define F(x) {x, sizeof x}

typedef struct {
    frame_ref frames[32];
    size_t frame_count;
    size_t frame_index;

    wc_write_kind kinds[32];
    uint8_t writes[32][WC_FRAME_MAX];
    size_t write_lens[32];
    size_t write_count;

    int lla_calls;
    int lla_level;
    bool lla_unavailable;

    char pair_address[64];
    int pair_calls;
} fake_transport;

static int fake_write(void *ctx, wc_write_kind kind, const uint8_t *data,
                      size_t len)
{
    fake_transport *fake = ctx;
    assert(fake->write_count < 32);
    assert(len <= WC_FRAME_MAX);
    fake->kinds[fake->write_count] = kind;
    memcpy(fake->writes[fake->write_count], data, len);
    fake->write_lens[fake->write_count] = len;
    fake->write_count++;
    return 0;
}

static int fake_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)timeout_ms;
    fake_transport *fake = ctx;
    if (fake->frame_index >= fake->frame_count) {
        return 0;
    }
    frame_ref frame = fake->frames[fake->frame_index++];
    assert(frame.len <= cap);
    memcpy(buf, frame.data, frame.len);
    return (int)frame.len;
}

static int fake_lla(void *ctx, uint8_t level)
{
    fake_transport *fake = ctx;
    fake->lla_calls++;
    fake->lla_level = level;
    return fake->lla_unavailable ? -1 : 0;
}

static int fake_pair(void *ctx, const char *address)
{
    fake_transport *fake = ctx;
    fake->pair_calls++;
    size_t i = 0;
    while (i + 1 < sizeof fake->pair_address && address[i] != '\0') {
        fake->pair_address[i] = address[i];
        i++;
    }
    fake->pair_address[i] = '\0';
    return 1;
}

static wc_session *make_session(fake_transport *fake, const frame_ref *frames,
                                size_t count, const uint8_t app_id[10])
{
    assert(count <= 32);
    if (count > 0) {
        memcpy(fake->frames, frames, sizeof(frames[0]) * count);
    }
    fake->frame_count = count;

    wc_transport transport;
    memset(&transport, 0, sizeof transport);
    transport.ctx = fake;
    transport.write = fake_write;
    transport.read = fake_read;
    transport.write_link_loss_alert = fake_lla;
    transport.pair = fake_pair;
    transport.address = "DA:02:44:21:46:51";
    return wc_session_new(&transport, app_id, 50);
}

static void test_gbx100_handshake(void)
{
    static const uint8_t app_id[10] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
    fake_transport fake;
    memset(&fake, 0, sizeof fake);
    frame_ref frames[] = {
        F(NAME),       F(FEATURES), F(BLE_SETTINGS), F(ADVERTISE),
        F(CONNECTION), F(MODULE),   F(CONDITION),    F(VERSION),
        F(SD_START),   F(SD_DONE),
    };
    wc_session *session = make_session(&fake, frames, 10, app_id);
    assert(session != NULL);

    wc_identity identity;
    int rc = wc_session_handshake(session, &identity, true, 50);
    assert(rc == WC_SESSION_OK);
    assert(strcmp(identity.name, "CASIO GBX-100") == 0);
    assert(identity.model != NULL);
    assert(strcmp(identity.model->name, "GBX_100") == 0);
    assert(identity.has_condition);
    assert(identity.condition.mtu == 442);
    assert(identity.pairing == WC_PAIRING_BOND_REQUESTED);
    assert(fake.pair_calls == 1);
    assert(strcmp(fake.pair_address, "DA:02:44:21:46:51") == 0);

    assert(fake.write_count == 9);
    assert(fake.kinds[0] == WC_WRITE_REQUEST);
    assert(fake.kinds[1] == WC_WRITE_REQUEST);
    assert(fake.kinds[2] == WC_WRITE_ALL);
    assert(fake.writes[0][0] == 0x23);
    assert(fake.writes[1][0] == 0x10);

    assert(fake.write_lens[2] == WC_LEN_APP_INFO);
    assert(fake.writes[2][0] == 0x22);
    assert(memcmp(fake.writes[2] + 1, app_id, sizeof app_id) == 0);
    assert(fake.writes[2][11] == 2);

    for (size_t i = 3; i < 9; i++) {
        assert(fake.kinds[i] == WC_WRITE_REQUEST);
    }
    wc_session_free(session);
}

static void test_service_discovery_is_parked(void)
{
    static const uint8_t app_id[10] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
    fake_transport fake;
    memset(&fake, 0, sizeof fake);
    frame_ref frames[] = {
        F(NAME),       F(FEATURES),  F(BLE_SETTINGS), F(ADVERTISE),
        F(CONNECTION), F(MODULE),    F(SD_START),     F(CONDITION),
        F(VERSION),    F(SD_DONE),
    };
    wc_session *session = make_session(&fake, frames, 10, app_id);
    assert(session != NULL);

    wc_identity identity;
    int rc = wc_session_handshake(session, &identity, true, 50);
    assert(rc == WC_SESSION_OK);
    assert(identity.pairing == WC_PAIRING_BOND_REQUESTED);
    assert(strcmp(identity.name, "CASIO GBX-100") == 0);
    wc_session_free(session);
}

static void test_no_service_discovery(void)
{
    static const uint8_t app_id[10] = {3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
    fake_transport fake;
    memset(&fake, 0, sizeof fake);
    frame_ref frames[] = {
        F(NAME),        F(FEATURES), F(BLE_SETTINGS), F(ADVERTISE),
        F(CONNECTION),  F(MODULE),   F(CONDITION),    F(VERSION),
    };
    wc_session *session = make_session(&fake, frames, 8, app_id);
    assert(session != NULL);

    wc_identity identity;
    int rc = wc_session_handshake(session, &identity, true, 50);
    assert(rc == WC_SESSION_OK);
    assert(identity.pairing == WC_PAIRING_NOT_OBSERVED);
    wc_session_free(session);
}

static void test_rejection_is_reported(void)
{
    static const uint8_t app_id[10] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    fake_transport fake;
    memset(&fake, 0, sizeof fake);
    frame_ref frames[] = {F(ERROR)};
    wc_session *session = make_session(&fake, frames, 1, app_id);
    assert(session != NULL);

    wc_identity identity;
    int rc = wc_session_handshake(session, &identity, false, 50);
    assert(rc == WC_SESSION_ERR_REJECTED);
    wc_error error;
    assert(wc_session_last_error(session, &error));
    assert(error.class_id == 0x09);
    assert(error.status == 0x81);
    wc_session_free(session);
}

static void test_unknown_model_uses_link_loss_alert(void)
{
    static const uint8_t app_id[10] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
    fake_transport fake;
    memset(&fake, 0, sizeof fake);
    frame_ref frames[] = {
        F(UNKNOWN_NAME), F(FEATURES), F(BLE_SETTINGS), F(ADVERTISE),
        F(CONNECTION),   F(MODULE),   F(CONDITION),    F(VERSION),
        F(SD_START),     F(SD_DONE),
    };
    wc_session *session = make_session(&fake, frames, 10, app_id);
    assert(session != NULL);

    wc_identity identity;
    int rc = wc_session_handshake(session, &identity, true, 50);
    assert(rc == WC_SESSION_OK);
    assert(identity.model == NULL);
    assert(identity.pairing == WC_PAIRING_LINK_LOSS_ALERT);
    assert(fake.lla_calls == 1);
    assert(fake.lla_level == 1);
    assert(fake.pair_calls == 0);
    wc_session_free(session);
}

static void test_missing_link_loss_alert_characteristic(void)
{
    static const uint8_t app_id[10] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
    fake_transport fake;
    memset(&fake, 0, sizeof fake);
    fake.lla_unavailable = true;
    frame_ref frames[] = {
        F(UNKNOWN_NAME), F(FEATURES), F(BLE_SETTINGS), F(ADVERTISE),
        F(CONNECTION),   F(MODULE),   F(CONDITION),    F(VERSION),
        F(SD_START),     F(SD_DONE),
    };
    wc_session *session = make_session(&fake, frames, 10, app_id);
    assert(session != NULL);

    wc_identity identity;
    int rc = wc_session_handshake(session, &identity, true, 50);
    assert(rc == WC_SESSION_OK);
    assert(identity.pairing == WC_PAIRING_LINK_LOSS_ALERT_UNAVAILABLE);
    wc_session_free(session);
}

static void test_timeout_on_empty_transport(void)
{
    static const uint8_t app_id[10] = {6, 6, 6, 6, 6, 6, 6, 6, 6, 6};
    fake_transport fake;
    memset(&fake, 0, sizeof fake);
    wc_session *session = make_session(&fake, NULL, 0, app_id);
    assert(session != NULL);

    wc_identity identity;
    int rc = wc_session_handshake(session, &identity, false, 50);
    assert(rc == WC_SESSION_ERR_TIMEOUT);
    wc_session_free(session);
}

static void test_read_helpers(void)
{
    static const uint8_t app_id[10] = {8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
    fake_transport fake;
    memset(&fake, 0, sizeof fake);
    frame_ref frames[] = {F(BASIC_SETTING), F(PROGRAM_TIMER)};
    wc_session *session = make_session(&fake, frames, 2, app_id);
    assert(session != NULL);

    wc_basic_setting setting;
    assert(wc_session_read_basic_settings(session, &setting) == WC_SESSION_OK);
    assert(setting.flags == 0x44);
    assert(setting.auto_light_off == 1);

    wc_program_timer timer;
    assert(wc_session_read_program_timer(session, &timer) == WC_SESSION_OK);
    assert(timer.repeat == 1);
    assert(timer.workouts[0].seconds == 25);
    assert(timer.workouts[1].seconds == 5);
    wc_session_free(session);
}

static void test_invalid_arguments(void)
{
    uint8_t app_id[10] = {0};
    wc_transport transport;
    memset(&transport, 0, sizeof transport);
    assert(wc_session_new(NULL, app_id, 50) == NULL);
    assert(wc_session_new(&transport, app_id, 50) == NULL);
    assert(wc_session_new(&transport, NULL, 50) == NULL);
}

int main(void)
{
    test_gbx100_handshake();
    test_service_discovery_is_parked();
    test_no_service_discovery();
    test_rejection_is_reported();
    test_unknown_model_uses_link_loss_alert();
    test_missing_link_loss_alert_characteristic();
    test_timeout_on_empty_transport();
    test_read_helpers();
    test_invalid_arguments();
    return 0;
}
