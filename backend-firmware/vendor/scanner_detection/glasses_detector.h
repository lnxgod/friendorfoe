/**
 * @file glasses_detector.h
 * @brief BLE smart glasses and privacy device detector.
 *
 * Identifies smart glasses, body cameras, and other privacy-intrusion
 * devices by matching BLE advertisement data against a database of known
 * manufacturer Company IDs, service UUIDs, and device name prefixes.
 *
 * Gated by CONFIG_FOF_GLASSES_DETECTION (KConfig, default y).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum tracked glasses devices */
#define MAX_GLASSES_DEVICES 20

/** Detection result for a smart glasses / privacy device */
typedef struct {
    char     device_name[32];    /**< BLE advertised name (may be empty) */
    char     device_type[24];    /**< "Smart Glasses", "Body Camera", etc. */
    char     manufacturer[24];   /**< "Meta", "Snap", "Bose", etc. */
    uint8_t  mac[6];             /**< BLE MAC address */
    int8_t   rssi;               /**< Signal strength (dBm) */
    float    confidence;         /**< Match confidence 0.0-1.0 */
    bool     has_camera;         /**< Device is camera-equipped */
    char     match_reason[32];   /**< e.g. "mfr_cid:0x01AB" or "name:Spectacles" */
    int64_t  first_seen_ms;
    int64_t  last_seen_ms;
} glasses_detection_t;

/**
 * Check a BLE advertisement for smart glasses / privacy device signatures.
 *
 * @param mac          6-byte BLE MAC address
 * @param adv_name     Device name from advertisement (NULL if not present)
 * @param adv_name_len Length of adv_name
 * @param mfr_data     Manufacturer-specific data (NULL if not present)
 * @param mfr_data_len Length of mfr_data (first 2 bytes = Company ID LE)
 * @param svc_uuid16s  Array of 16-bit service UUIDs found in advertisement
 * @param svc_uuid16_count Number of 16-bit service UUIDs
 * @param appearance   GAP Appearance value (0 if not present)
 * @param rssi         RSSI of the advertisement
 * @param out          Output detection result (populated on match)
 * @return true if a match was found
 */
bool glasses_check_advertisement(
    const uint8_t mac[6],
    const char *adv_name,
    int adv_name_len,
    const uint8_t *mfr_data,
    int mfr_data_len,
    const uint16_t *svc_uuid16s,
    int svc_uuid16_count,
    uint16_t appearance,
    int8_t rssi,
    glasses_detection_t *out
);

/**
 * Check if glasses detection is enabled at runtime (NVS).
 * Call after NVS is initialized.
 * @return true if enabled
 */
bool glasses_detection_is_enabled(void);

/**
 * Set glasses detection enabled/disabled at runtime (writes NVS).
 */
void glasses_detection_set_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
