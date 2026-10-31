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

/* Called from inside a blocking scan/connect; return true to abort it. */
typedef bool (*wc_cancel_fn)(void *ctx);

/* Opens the system bus. Returns NULL if BlueZ/sd-bus is unavailable. */
wc_bluez *wc_bluez_new(void);

/* Scans for the watch (by address when given, otherwise by `name` or the Casio
 * prefixes), connects, resolves GATT and subscribes to notifications.
 * `scan_timeout_ms` covers one scan window; the caller may retry. */
int wc_bluez_connect(wc_bluez *bluez, const char *name, const char *address,
                     uint32_t scan_timeout_ms);

/* Aborts a blocking wc_bluez_connect() within about 100 ms when `cancel`
 * returns true. Pass NULL to detach. The callback still runs on the calling
 * thread, so it must only read atomics or a flag the caller owns. */
void wc_bluez_set_cancel(wc_bluez *bluez, wc_cancel_fn cancel, void *ctx);

/* Processes pending D-Bus events for up to `timeout_ms` (0 polls). Call this
 * while connected and idle so a Device1 disconnect is noticed. */
int wc_bluez_pump(wc_bluez *bluez, uint32_t timeout_ms);

/* True when BlueZ reported Device1 Connected=false for this device. It is
 * cleared by the next wc_bluez_connect() on the same instance. */
bool wc_bluez_link_lost(const wc_bluez *bluez);

/* Removes BlueZ's cached device object and its bond. Pass the address to
 * remove, or NULL for the device this instance last connected to. Used to
 * recover from a stale bond after the watch was reset or re-paired. */
int wc_bluez_forget(wc_bluez *bluez, const char *address);

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
