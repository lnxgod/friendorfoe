#pragma once

/**
 * Friend or Foe -- Promiscuous WiFi Scanner
 *
 * Captures raw WiFi beacon frames in promiscuous mode, parsing:
 * - DJI vendor-specific Information Elements (DroneID)
 * - ASTM F3411 WiFi Beacon Remote ID
 * - Known drone SSID patterns
 * - Known drone manufacturer OUI prefixes
 *
 * ESP32-S3: 2.4 GHz only (ch 1-13), dual-core, pinned to Core 0.
 * ESP32-C5: Dual-band 2.4 + 5 GHz (ch 1-13 + 36-165), single-core.
 *
 * Channel-hops with ~100ms dwell time per channel. On dual-band builds,
 * channels are interleaved (one 2.4 GHz, one 5 GHz) so neither band
 * is starved.
 */

#include "detection_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the WiFi subsystem in promiscuous mode.
 *
 * @param detection_queue FreeRTOS queue for drone_detection_t results
 */
void wifi_scanner_init(QueueHandle_t detection_queue);

/**
 * Start the WiFi scan task (channel hopping + frame processing).
 * On dual-core (S3) the task is pinned to Core 0; on single-core (C5)
 * it runs without core affinity.
 */
void wifi_scanner_start(void);

/**
 * Set the WiFi channel manually (overrides automatic hopping for one cycle).
 *
 * @param channel WiFi channel number (1-13 for 2.4 GHz, 36-165 for 5 GHz)
 */
void wifi_scanner_set_channel(uint16_t channel);

/**
 * Get the current WiFi channel.
 *
 * @return Current channel number (1-13 or 36-165)
 */
uint16_t wifi_scanner_get_channel(void);

/**
 * Check if 5 GHz scanning is enabled (compile-time).
 *
 * @return true if built with CONFIG_SCANNER_5GHZ_ENABLED
 */
bool wifi_scanner_is_5ghz_enabled(void);

/**
 * Lock onto a specific channel for intensive data capture.
 * During lock-on, the scanner stops channel hopping and captures
 * all frames on the target channel.
 *
 * @param channel     WiFi channel to lock onto (1-13)
 * @param bssid       Target BSSID to focus on (NULL = capture all)
 * @param duration_s  Lock-on duration: 30, 60, or 90 seconds
 * @return true if lock-on activated
 */
bool wifi_scanner_lockon(uint8_t channel, const char *bssid, int duration_s);

/**
 * Cancel an active lock-on and resume normal scanning.
 */
void wifi_scanner_lockon_cancel(void);

/**
 * Check if lock-on mode is currently active.
 */
bool wifi_scanner_is_locked_on(void);

#ifdef __cplusplus
}
#endif
