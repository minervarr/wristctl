/* App notifications (class 0x30).
 *
 * This payload is variable length, so it is a procedure rather than one of the
 * fixed-layout classes in spec/classes.json. The layout is derived from
 * gshock_api and recorded in GSHOCK_DECOMP/work/protocol/ble-protocol.md:
 *
 *   00 00 00 00 00 01
 *   type (1)
 *   timestamp (15 ASCII, YYYYMMDDTHHMMSS)
 *   app     [len][00][utf8]
 *   title   [len][00][utf8]
 *   short   [len][00][utf8]
 *   text    [len][00][utf8]
 *
 * Every byte is XORed with 0xFF on the wire. Fields are limited to 255 bytes
 * each by the length byte; WC_NOTIFY_MAX is a transport-sized guard, not a
 * protocol limit. The GBX-100 negotiated MTU 442, so keep a packet under
 * MTU-3 bytes unless the transport fragments it.
 */
#ifndef WRISTCTL_NOTIFY_H
#define WRISTCTL_NOTIFY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WC_NOTIFY_HEADER_LEN 6
#define WC_NOTIFY_STAMP_LEN 15
#define WC_NOTIFY_FIELD_MAX 255
#define WC_NOTIFY_MAX 512

typedef struct {
    uint8_t type;
    char timestamp[WC_NOTIFY_STAMP_LEN + 1]; /* NUL-terminated */
    char app[WC_NOTIFY_FIELD_MAX + 1];
    char title[WC_NOTIFY_FIELD_MAX + 1];
    char short_text[WC_NOTIFY_FIELD_MAX + 1];
    char text[WC_NOTIFY_FIELD_MAX + 1];
} wc_notification;

/* Encodes `value` into `out`, XOR-obfuscated as it goes on the wire.
 * Returns 0, WC_NOTIFY_ERR_ARG, WC_NOTIFY_ERR_CAPACITY or
 * WC_NOTIFY_ERR_RANGE. `out_len` receives the packet length. */
int wc_notification_encode(const wc_notification *value, uint8_t *out,
                           size_t cap, size_t *out_len);

/* Decodes a wire packet (XOR included). The decoded struct is zeroed first,
 * so unset fields are empty strings. */
int wc_notification_decode(const uint8_t *in, size_t len, wc_notification *out);

enum {
    WC_NOTIFY_OK = 0,
    WC_NOTIFY_ERR_ARG = -1,
    WC_NOTIFY_ERR_CAPACITY = -2,
    WC_NOTIFY_ERR_RANGE = -3
};

#ifdef __cplusplus
}
#endif

#endif /* WRISTCTL_NOTIFY_H */
