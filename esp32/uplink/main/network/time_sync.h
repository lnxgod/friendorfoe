#pragma once

/**
 * Friend or Foe -- Uplink SNTP Time Synchronization
 *
 * Synchronizes the ESP32's system clock via SNTP after WiFi connects.
 * Uses pool.ntp.org and time.google.com as NTP servers.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize and start SNTP time synchronization.
 * Should be called after WiFi connection is established.
 */
void time_sync_init(void);

/**
 * Check whether the system clock has been synchronized via SNTP.
 */
bool time_sync_is_synced(void);

/**
 * Get the current time as epoch milliseconds.
 * Returns 0 if time has not yet been synchronized.
 */
int64_t time_sync_get_epoch_ms(void);

#ifdef __cplusplus
}
#endif
