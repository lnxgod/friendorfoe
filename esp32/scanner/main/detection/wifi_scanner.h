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
 * Channel-hops with BLE-safe passive dwell timing.
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

/**
 * Get current attack/anomaly counters (delta since last reset).
 * Any output pointer may be NULL to skip that field.
 */
void wifi_scanner_get_attack_counters(uint16_t *deauth, uint16_t *disassoc,
                                       uint16_t *auth, bool *flood,
                                       bool *bcn_spam);

/**
 * Reset attack counters to zero (call after reporting).
 */
void wifi_scanner_reset_attack_counters(void);

/**
 * Get the frame control subtype histogram (16 entries, indexed by subtype 0-15).
 */
void wifi_scanner_get_fc_histogram(uint32_t out[16]);

/**
 * Reset the frame control histogram to zero.
 */
void wifi_scanner_reset_fc_histogram(void);

/** Pause WiFi scanning (disable promiscuous mode). Used during OTA. */
void wifi_scanner_pause(void);

/** Resume WiFi scanning (re-enable promiscuous mode). */
void wifi_scanner_resume(void);

#ifdef __cplusplus
}
#endif
