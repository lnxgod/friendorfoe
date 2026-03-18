#pragma once

/**
 * Friend or Foe -- ESP32-S3 Promiscuous WiFi Scanner
 *
 * Captures raw WiFi beacon frames in promiscuous mode, parsing:
 * - DJI vendor-specific Information Elements (DroneID)
 * - ASTM F3411 WiFi Beacon Remote ID
 * - Known drone SSID patterns
 * - Known drone manufacturer OUI prefixes
 *
 * Runs on Core 0 alongside the WiFi driver ISRs for lowest latency.
 * Channel-hops across 2.4 GHz channels 1-13 with ~100ms dwell time.
 */

#include "detection_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

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
 * Creates a FreeRTOS task pinned to Core 0.
 */
void wifi_scanner_start(void);

/**
 * Set the WiFi channel manually (overrides automatic hopping for one cycle).
 *
 * @param channel WiFi channel number (1-13)
 */
void wifi_scanner_set_channel(uint8_t channel);

/**
 * Get the current WiFi channel.
 *
 * @return Current channel number (1-13)
 */
uint8_t wifi_scanner_get_channel(void);

#ifdef __cplusplus
}
#endif
