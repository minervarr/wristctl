/* Base definitions shared by every generated codec. Hand-written. */
#ifndef WRISTCTL_PROTO_H
#define WRISTCTL_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* Return codes. Zero is success; every failure is negative so that callers can
 * switch once without inspecting errno. */
enum {
    WC_OK = 0,
    WC_ERR_ARG = -1,      /* NULL pointer */
    WC_ERR_SHORT = -2,    /* frame shorter than the class length */
    WC_ERR_CLASS = -3,    /* class byte does not match */
    WC_ERR_RANGE = -4,    /* value outside the spec range on encode */
    WC_ERR_CAPACITY = -5, /* output buffer too small */
    WC_ERR_BCD = -6,      /* BCD value outside 0..99 */
    WC_ERR_LENGTH = -7,   /* string/bytes field longer than the frame field */
    WC_ERR_CONST = -8     /* fixed byte in the frame has an unexpected value */
};

typedef enum {
    WC_DIR_READ = 1,
    WC_DIR_WRITE = 2,
    WC_DIR_READWRITE = 3
} wc_direction;

typedef enum {
    WC_CONFIDENCE_OBSERVED = 0,
    WC_CONFIDENCE_DERIVED = 1,
    WC_CONFIDENCE_INFERRED = 2
} wc_confidence;

#endif /* WRISTCTL_PROTO_H */
