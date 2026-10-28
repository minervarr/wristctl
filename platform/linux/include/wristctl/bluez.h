/* BlueZ transport for the watch session (Linux, sd-bus).
 *
 * One instance owns one system-bus connection and one device. It is
 * single-threaded: the caller writes, then blocks in next_frame(), which pumps
 * the bus and dispatches PropertiesChanged itself. Run it on a worker thread;
 * never call it from the render thread.
 *
 * GATT layout (GBX-100, and the vendor app's own service):
 *   service      26eb000d-b012-49a8-b1f8-394fb2032b0f
 *   request      26eb002c-...  write without response
 *   settings     26eb002d-...  write with response
 *   notification 26eb0030-...  write without response
 *   link loss    00002a06-...  standard alert level
 */
#ifndef WRISTCTL_BLUEZ_H
#define WRISTCTL_BLUEZ_H

#include <stdbool.h>
#include <stdint.h>

#include "wristctl/session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wc_bluez wc_bluez;

/* Opens the system bus. Returns NULL if BlueZ/sd-bus is unavailable. */
wc_bluez *wc_bluez_new(void);

/* Scans for the watch (by address when given, otherwise by `name` or the Casio
 * prefixes), connects, resolves GATT and subscribes to notifications.
 * `scan_timeout_ms` covers one scan window; the caller may retry. */
int wc_bluez_connect(wc_bluez *bluez, const char *name, const char *address,
                     uint32_t scan_timeout_ms);

/* Valid after a successful wc_bluez_connect(). The session copies this. */
const wc_transport *wc_bluez_transport(wc_bluez *bluez);

const char *wc_bluez_address(const wc_bluez *bluez);
const char *wc_bluez_name(const wc_bluez *bluez);
const char *wc_bluez_error(const wc_bluez *bluez);

/* Disconnects and releases the bus. Safe on a half-initialised instance. */
void wc_bluez_free(wc_bluez *bluez);

#ifdef __cplusplus
}
#endif

#endif /* WRISTCTL_BLUEZ_H */
