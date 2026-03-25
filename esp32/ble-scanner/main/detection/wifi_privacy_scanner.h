/**
 * @file wifi_privacy_scanner.h
 * @brief WiFi scanner for privacy-threatening SSIDs and Meta hotspot detection.
 *
 * Performs periodic WiFi scans looking for hidden camera SSIDs,
 * Meta glasses video transfer hotspots, and other suspicious APs.
 *
 * Designed for time-sliced operation: BLE pauses, WiFi scans, BLE resumes.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** WiFi privacy detection result */
typedef struct {
    char ssid[33];
    char bssid[18];
    int8_t rssi;
    char device_type[24];   /* "Hidden Camera", "Meta Transfer", etc. */
    char manufacturer[24];
    float confidence;
    bool has_camera;
    bool is_open;           /* true if no encryption (suspicious) */
} wifi_privacy_result_t;

#define MAX_WIFI_PRIVACY_RESULTS 10

/**
 * Initialize WiFi for scanning (call once at startup).
 * Sets up WiFi in STA mode without connecting.
 */
void wifi_privacy_init(void);

/**
 * Run a single WiFi scan and check results against privacy SSID database.
 * Blocks for ~2-3 seconds while scan completes.
 *
 * @param results  Output array of matched results
 * @param max_results  Max entries in array
 * @return Number of matches found (0 if none)
 */
int wifi_privacy_scan(wifi_privacy_result_t *results, int max_results);

/**
 * Check if a Meta WiFi transfer hotspot was detected in the last scan.
 * @return true if Meta hotspot found
 */
bool wifi_privacy_meta_transfer_detected(void);

/**
 * Deinitialize WiFi (free resources).
 */
void wifi_privacy_deinit(void);

#ifdef __cplusplus
}
#endif
