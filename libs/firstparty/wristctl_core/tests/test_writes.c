#undef NDEBUG

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "wristctl/notify.h"
#include "wristctl/session.h"

typedef struct {
    uint8_t data[WC_FRAME_MAX];
    size_t len;
    wc_write_kind kind;
} captured_write;

typedef struct {
    captured_write writes[16];
    size_t write_count;
    int fail_write_at;
} fake_transport;

static int fake_write(void *ctx, wc_write_kind kind, const uint8_t *data,
                      size_t len)
{
    fake_transport *fake = ctx;
    if (fake->fail_write_at >= 0 &&
        fake->write_count == (size_t)fake->fail_write_at) {
        return -1;
    }
    assert(fake->write_count < 16);
    assert(len <= WC_FRAME_MAX);
    captured_write *write = &fake->writes[fake->write_count++];
    memcpy(write->data, data, len);
    write->len = len;
    write->kind = kind;
    return 0;
}

static int fake_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)ctx;
    (void)buf;
    (void)cap;
    (void)timeout_ms;
    return 0;
}

static wc_session *make_session(fake_transport *fake)
{
    static const uint8_t app_id[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    wc_transport transport;
    memset(&transport, 0, sizeof transport);
    transport.ctx = fake;
    transport.write = fake_write;
    transport.read = fake_read;
    transport.address = "DA:02:44:21:46:51";
    return wc_session_new(&transport, app_id, 50);
}

static void test_alarm_write(void)
{
    fake_transport fake;
    memset(&fake, 0, sizeof fake);
    fake.fail_write_at = -1;
    wc_session *session = make_session(&fake);
    assert(session != NULL);

    wc_alarm_first first;
    memset(&first, 0, sizeof first);
    first.enabled = 1;
    first.hourly_chime = 1;
    first.constant = 0x40;
    first.hour = 7;
    first.minute = 30;

    wc_alarm_rest rest;
    memset(&rest, 0, sizeof rest);
    rest.alarms[0].enabled = 1;
    rest.alarms[0].constant = 0x40;
    rest.alarms[0].hour = 8;
    rest.alarms[1].hour = 9;
    rest.alarms[1].minute = 5;

    assert(wc_session_write_alarms(session, &first, &rest) == WC_SESSION_OK);
    assert(fake.write_count == 2);
    assert(fake.writes[0].kind == WC_WRITE_ALL);
    assert(fake.writes[0].len == WC_LEN_ALARM_FIRST);
    const uint8_t expected_first[] = {0x15, 0xc0, 0x40, 0x07, 0x1e};
    assert(memcmp(fake.writes[0].data, expected_first,
                  sizeof expected_first) == 0);
    assert(fake.writes[1].len == WC_LEN_ALARM_REST);
    assert(fake.writes[1].data[0] == 0x16);
    assert(fake.writes[1].data[1] == 0x40);
    assert(fake.writes[1].data[2] == 0x40);
    assert(fake.writes[1].data[3] == 0x08);
    assert(fake.writes[1].data[4] == 0x00);
    assert(fake.writes[1].data[5] == 0x00);
    assert(fake.writes[1].data[7] == 0x09);
    assert(fake.writes[1].data[8] == 0x05);
    wc_session_free(session);
}

static void test_basic_settings_write_preserves_unknown_bits(void)
{
    fake_transport fake;
    memset(&fake, 0, sizeof fake);
    fake.fail_write_at = -1;
    wc_session *session = make_session(&fake);
    assert(session != NULL);

    wc_basic_setting settings;
    memset(&settings, 0, sizeof settings);
    settings.flags = 0x40; /* an unknown bit the caller read */
    settings.time_format_24h = 1;
    settings.auto_light_off = 1;
    settings.light_duration = 1;
    settings.date_format_ddmm = 1;
    settings.language = 2;
    settings.reserved_tail[0] = 0xaa;

    assert(wc_session_write_basic_settings(session, &settings) ==
           WC_SESSION_OK);
    assert(fake.write_count == 1);
    assert(fake.writes[0].len == WC_LEN_BASIC_SETTING);
    assert(fake.writes[0].data[0] == 0x13);
    assert(fake.writes[0].data[1] == 0x45);
    assert(fake.writes[0].data[2] == 1);
    assert(fake.writes[0].data[4] == 1);
    assert(fake.writes[0].data[5] == 2);
    assert(fake.writes[0].data[6] == 0xaa);
    wc_session_free(session);
}

static void test_program_timer_write(void)
{
    fake_transport fake;
    memset(&fake, 0, sizeof fake);
    fake.fail_write_at = -1;
    wc_session *session = make_session(&fake);
    assert(session != NULL);

    wc_program_timer timer;
    memset(&timer, 0, sizeof timer);
    timer.repeat = 2;
    timer.workouts[0].minutes = 5;
    timer.workouts[0].seconds = 30;
    timer.workouts[0].beep = 1;
    timer.workouts[1].minutes = 1;
    timer.workouts[1].seconds = 0;

    assert(wc_session_write_program_timer(session, &timer) == WC_SESSION_OK);
    assert(fake.write_count == 1);
    assert(fake.writes[0].len == WC_LEN_PROGRAM_TIMER);
    assert(fake.writes[0].data[0] == 0x2a);
    assert(fake.writes[0].data[1] == 2);
    assert(fake.writes[0].data[2] == 0x05);
    assert(fake.writes[0].data[3] == 0x30);
    assert(fake.writes[0].data[4] == 1);
    assert(fake.writes[0].data[5] == 0x01);
    assert(fake.writes[0].data[6] == 0x00);
    wc_session_free(session);
}

static void test_watch_name_write(void)
{
    fake_transport fake;
    memset(&fake, 0, sizeof fake);
    fake.fail_write_at = -1;
    wc_session *session = make_session(&fake);
    assert(session != NULL);

    assert(wc_session_write_watch_name(session, "GBX") == WC_SESSION_OK);
    assert(fake.write_count == 1);
    assert(fake.writes[0].len == WC_LEN_WATCH_NAME);
    assert(fake.writes[0].data[0] == 0x23);
    assert(memcmp(fake.writes[0].data + 1, "GBX", 3) == 0);
    assert(fake.writes[0].data[4] == 0x00);

    assert(wc_session_write_watch_name(session, "1234567890123456789") ==
           WC_SESSION_OK);
    assert(wc_session_write_watch_name(session, "12345678901234567890") ==
           WC_SESSION_ERR_INVALID);
    wc_session_free(session);
}

static void test_dst_and_city_writes(void)
{
    fake_transport fake;
    memset(&fake, 0, sizeof fake);
    fake.fail_write_at = -1;
    wc_session *session = make_session(&fake);
    assert(session != NULL);

    wc_dst_watch_state state;
    memset(&state, 0, sizeof state);
    state.watch_no_world = 1;
    state.state_home = 2;
    state.dst_auto_home = true;
    state.state_world = 2;
    state.dst_auto_world = true;
    state.city_home = 30389;
    state.city_extra_1 = 0xffff;
    state.city_extra_2 = 0xffff;
    state.city_extra_3 = 0xffff;
    assert(wc_session_write_dst_watch_state(session, &state) == WC_SESSION_OK);

    wc_dst_setting setting;
    memset(&setting, 0, sizeof setting);
    setting.city = 30389;
    assert(wc_session_write_dst_setting(session, &setting) == WC_SESSION_OK);

    wc_city_name name;
    memset(&name, 0, sizeof name);
    memcpy(name.name, "UTC", 3);
    assert(wc_session_write_city_name(session, &name) == WC_SESSION_OK);

    const uint8_t expected_state[] = {0x1d, 0x00, 0x01, 0x02, 0x02,
                                      0xb5, 0x76, 0x00, 0x00, 0xff,
                                      0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t expected_utc[] = {0x1e, 0x00, 0xb5, 0x76, 0x00, 0x00, 0x00};
    const uint8_t expected_name[] = {0x1f, 0x00, 0x55, 0x54, 0x43, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    assert(fake.write_count == 3);
    for (size_t i = 0; i < fake.write_count; i++) {
        assert(fake.writes[i].kind == WC_WRITE_ALL);
    }
    assert(fake.writes[0].len == sizeof expected_state);
    assert(memcmp(fake.writes[0].data, expected_state, sizeof expected_state) == 0);
    assert(fake.writes[1].len == sizeof expected_utc);
    assert(memcmp(fake.writes[1].data, expected_utc, sizeof expected_utc) == 0);
    assert(fake.writes[2].len == sizeof expected_name);
    assert(memcmp(fake.writes[2].data, expected_name, sizeof expected_name) == 0);
    assert(wc_session_write_dst_watch_state(session, NULL) ==
           WC_SESSION_ERR_INVALID);
    assert(wc_session_write_dst_setting(session, NULL) == WC_SESSION_ERR_INVALID);
    assert(wc_session_write_city_name(session, NULL) == WC_SESSION_ERR_INVALID);
    wc_session_free(session);
}

static void test_notification_round_trip(void)
{
    wc_notification value;
    memset(&value, 0, sizeof value);
    value.type = 1;
    memcpy(value.timestamp, "20261027T164109", WC_NOTIFY_STAMP_LEN);
    strcpy(value.app, "Test");
    strcpy(value.title, "Hello");
    strcpy(value.short_text, "Hi");
    strcpy(value.text, "World");

    uint8_t packet[WC_NOTIFY_MAX];
    size_t length = 0;
    assert(wc_notification_encode(&value, packet, sizeof packet, &length) ==
           WC_NOTIFY_OK);
    assert(length > WC_NOTIFY_HEADER_LEN + WC_NOTIFY_STAMP_LEN);
    assert(packet[0] == 0xff); /* header 0x00 XOR 0xFF */

    wc_notification decoded;
    assert(wc_notification_decode(packet, length, &decoded) == WC_NOTIFY_OK);
    assert(decoded.type == 1);
    assert(strcmp(decoded.timestamp, "20261027T164109") == 0);
    assert(strcmp(decoded.app, "Test") == 0);
    assert(strcmp(decoded.title, "Hello") == 0);
    assert(strcmp(decoded.short_text, "Hi") == 0);
    assert(strcmp(decoded.text, "World") == 0);

    value.timestamp[0] = '\0';
    assert(wc_notification_encode(&value, packet, sizeof packet, &length) ==
           WC_NOTIFY_ERR_RANGE);
}

static void test_notification_exact_packet_and_validation(void)
{
    wc_notification value;
    memset(&value, 0, sizeof value);
    value.type = 1;
    memcpy(value.timestamp, "20261027T164109", WC_NOTIFY_STAMP_LEN);
    strcpy(value.app, "Test");
    strcpy(value.title, "Hello");
    strcpy(value.text, "World");

    static const uint8_t expected[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0xfe, 0xcd, 0xcf, 0xcd, 0xc9,
        0xce, 0xcf, 0xcd, 0xc8, 0xab, 0xce, 0xc9, 0xcb, 0xce, 0xcf, 0xc6,
        0xfb, 0xff, 0xab, 0x9a, 0x8c, 0x8b, 0xfa, 0xff, 0xb7, 0x9a, 0x93,
        0x93, 0x90, 0xff, 0xff, 0xfa, 0xff, 0xa8, 0x90, 0x8d, 0x93, 0x9b};
    uint8_t packet[WC_NOTIFY_MAX];
    size_t length = 0;
    assert(wc_notification_encode(&value, packet, sizeof packet, &length) ==
           WC_NOTIFY_OK);
    assert(length == sizeof expected);
    assert(memcmp(packet, expected, sizeof expected) == 0);
    assert(wc_notification_encode(&value, packet, length - 1, &length) ==
           WC_NOTIFY_ERR_CAPACITY);
    assert(wc_notification_encode(&value, packet, sizeof packet, &length) ==
           WC_NOTIFY_OK);

    wc_notification decoded;
    for (size_t cut = 0; cut < length; cut++) {
        assert(wc_notification_decode(packet, cut, &decoded) ==
               WC_NOTIFY_ERR_RANGE);
    }
    packet[length] = 0x00;
    assert(wc_notification_decode(packet, length + 1, &decoded) ==
           WC_NOTIFY_ERR_RANGE);
    packet[0] ^= 0x01;
    assert(wc_notification_decode(packet, length, &decoded) ==
           WC_NOTIFY_ERR_RANGE);
}

static void test_notification_send_uses_notification_kind(void)
{
    fake_transport fake;
    memset(&fake, 0, sizeof fake);
    fake.fail_write_at = -1;
    wc_session *session = make_session(&fake);
    assert(session != NULL);

    const uint8_t payload[] = {0xff, 0xfe, 0xfd};
    assert(wc_session_send_notification(session, payload, sizeof payload) ==
           WC_SESSION_OK);
    assert(fake.write_count == 1);
    assert(fake.writes[0].kind == WC_WRITE_NOTIFICATION);
    assert(fake.writes[0].len == sizeof payload);
    assert(memcmp(fake.writes[0].data, payload, sizeof payload) == 0);
    wc_session_free(session);
}

static void test_write_failure_is_reported(void)
{
    fake_transport fake;
    memset(&fake, 0, sizeof fake);
    fake.fail_write_at = 0;
    wc_session *session = make_session(&fake);
    assert(session != NULL);

    wc_basic_setting settings;
    memset(&settings, 0, sizeof settings);
    assert(wc_session_write_basic_settings(session, &settings) ==
           WC_SESSION_ERR_TRANSPORT);
    wc_session_free(session);
}

int main(void)
{
    test_alarm_write();
    test_basic_settings_write_preserves_unknown_bits();
    test_program_timer_write();
    test_watch_name_write();
    test_dst_and_city_writes();
    test_notification_round_trip();
    test_notification_exact_packet_and_validation();
    test_notification_send_uses_notification_kind();
    test_write_failure_is_reported();
    return 0;
}
