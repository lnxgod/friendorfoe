#pragma once

/**
 * Friend or Foe -- Uplink WiFi Station
 *
 * Manages a WiFi STA connection to the local access point for uploading
 * drone detections to the backend.  Automatically reconnects with
 * exponential backoff on disconnection.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize WiFi in STA mode and begin connecting.
 * Registers event handlers for connection management.
 * Must be called after esp_event_loop_create_default().
 */
void wifi_sta_init(void);

/**
 * Check whether WiFi is currently connected and has an IP address.
 */
bool wifi_sta_is_connected(void);

/**
 * Block until WiFi is connected or timeout expires.
 *
 * @param timeout_ms  Maximum time to wait in milliseconds.
 *                    Pass 0 to wait indefinitely.
 */
void wifi_sta_wait_connected(int timeout_ms);

/**
 * Check whether the uplink is running in standalone mode (AP-only).
 * Standalone mode is entered when no WiFi SSID has been configured.
 */
bool wifi_sta_is_standalone(void);

#ifdef __cplusplus
}
#endif
