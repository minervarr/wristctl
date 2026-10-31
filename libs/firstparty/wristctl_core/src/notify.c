#include "wristctl/notify.h"

#include <string.h>

static const uint8_t kHeader[WC_NOTIFY_HEADER_LEN] = {0x00, 0x00, 0x00,
                                                      0x00, 0x00, 0x01};
static const uint8_t kXor = 0xFF;

static int put_field(uint8_t *out, size_t cap, size_t *offset,
                     const char *value)
{
    const size_t length = strlen(value);
    if (length > WC_NOTIFY_FIELD_MAX) {
        return WC_NOTIFY_ERR_RANGE;
    }
    if (*offset + 2 + length > cap) {
        return WC_NOTIFY_ERR_CAPACITY;
    }
    out[*offset] = (uint8_t)length;
    out[*offset + 1] = 0x00;
    memcpy(out + *offset + 2, value, length);
    *offset += 2 + length;
    return WC_NOTIFY_OK;
}

int wc_notification_encode(const wc_notification *value, uint8_t *out,
                           size_t cap, size_t *out_len)
{
    if (value == NULL || out == NULL || out_len == NULL) {
        return WC_NOTIFY_ERR_ARG;
    }
    if (strlen(value->timestamp) != WC_NOTIFY_STAMP_LEN) {
        return WC_NOTIFY_ERR_RANGE;
    }

    size_t offset = 0;
    if (cap < WC_NOTIFY_HEADER_LEN + 1 + WC_NOTIFY_STAMP_LEN) {
        return WC_NOTIFY_ERR_CAPACITY;
    }
    memcpy(out, kHeader, sizeof kHeader);
    offset += sizeof kHeader;
    out[offset++] = value->type;
    memcpy(out + offset, value->timestamp, WC_NOTIFY_STAMP_LEN);
    offset += WC_NOTIFY_STAMP_LEN;

    int rc = put_field(out, cap, &offset, value->app);
    if (rc == WC_NOTIFY_OK) {
        rc = put_field(out, cap, &offset, value->title);
    }
    if (rc == WC_NOTIFY_OK) {
        rc = put_field(out, cap, &offset, value->short_text);
    }
    if (rc == WC_NOTIFY_OK) {
        rc = put_field(out, cap, &offset, value->text);
    }
    if (rc != WC_NOTIFY_OK) {
        return rc;
    }

    for (size_t i = 0; i < offset; i++) {
        out[i] ^= kXor;
    }
    *out_len = offset;
    return WC_NOTIFY_OK;
}

static int take_field(const uint8_t *in, size_t len, size_t *offset,
                      char *out, size_t cap)
{
    if (*offset + 2 > len) {
        return WC_NOTIFY_ERR_RANGE;
    }
    const size_t field_len = in[*offset];
    *offset += 2;
    if (*offset + field_len > len || field_len >= cap) {
        return WC_NOTIFY_ERR_RANGE;
    }
    memcpy(out, in + *offset, field_len);
    out[field_len] = '\0';
    *offset += field_len;
    return WC_NOTIFY_OK;
}

int wc_notification_decode(const uint8_t *in, size_t len, wc_notification *out)
{
    if (in == NULL || out == NULL) {
        return WC_NOTIFY_ERR_ARG;
    }
    if (len < WC_NOTIFY_HEADER_LEN + 1 + WC_NOTIFY_STAMP_LEN) {
        return WC_NOTIFY_ERR_RANGE;
    }

    uint8_t raw[WC_NOTIFY_MAX];
    if (len > sizeof raw) {
        return WC_NOTIFY_ERR_CAPACITY;
    }
    for (size_t i = 0; i < len; i++) {
        raw[i] = in[i] ^ kXor;
    }
    if (memcmp(raw, kHeader, sizeof kHeader) != 0) {
        return WC_NOTIFY_ERR_RANGE;
    }

    memset(out, 0, sizeof *out);
    size_t offset = WC_NOTIFY_HEADER_LEN;
    out->type = raw[offset++];
    memcpy(out->timestamp, raw + offset, WC_NOTIFY_STAMP_LEN);
    out->timestamp[WC_NOTIFY_STAMP_LEN] = '\0';
    offset += WC_NOTIFY_STAMP_LEN;

    int rc = take_field(raw, len, &offset, out->app, sizeof out->app);
    if (rc == WC_NOTIFY_OK) {
        rc = take_field(raw, len, &offset, out->title, sizeof out->title);
    }
    if (rc == WC_NOTIFY_OK) {
        rc = take_field(raw, len, &offset, out->short_text,
                        sizeof out->short_text);
    }
    if (rc == WC_NOTIFY_OK) {
        rc = take_field(raw, len, &offset, out->text, sizeof out->text);
    }
    if (rc == WC_NOTIFY_OK && offset != len) {
        return WC_NOTIFY_ERR_RANGE;
    }
    return rc;
}
