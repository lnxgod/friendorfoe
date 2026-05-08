#pragma once

/**
 * Friend or Foe -- BLE Remote ID Scanner (ASTM F3411)
 *
 * NimBLE-based BLE scanner that listens for OpenDroneID advertisements
 * on service UUID 0xFFFA. Parses ODID message packs (Basic ID, Location,
 * System, Operator ID, Self ID) and emits drone_detection_t results.
 *
 * Runs on Core 0 alongside the BT driver ISRs.
 */

#include "detection_types.h"
#include "open_drone_id_parser.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the NimBLE BLE scanner.
 *
 * @param detection_queue FreeRTOS queue for drone_detection_t results
 */
void ble_remote_id_init(QueueHandle_t detection_queue);

/**
 * Start BLE scanning for OpenDroneID advertisements.
 * Creates a FreeRTOS task pinned to Core 0.
 */
void ble_remote_id_start(void);

/**
 * Stop BLE scanning.
 */
void ble_remote_id_stop(void);

typedef struct {
    bool     ble_scanning;
    bool     ble_host_active;
    bool     ble_host_synced;
    uint32_t ble_adv_seen;
    uint32_t ble_fp_emit;
    uint32_t ble_meta_seen;
    uint32_t ble_tracker_seen;
    uint32_t ble_privacy_candidate_seen;
    uint32_t ble_near_unknown_seen;
    uint32_t ble_drop_rate;
    uint32_t ble_dbg_near_seen;
    int8_t   ble_dbg_near_rssi;
    char     ble_dbg_near_label[24];
    char     ble_dbg_near_name[32];
    char     ble_dbg_near_reason[32];
    uint16_t ble_dbg_near_cid;
    uint16_t ble_dbg_near_svc0;
    uint8_t  ble_dbg_near_svc_count;
    uint8_t  ble_dbg_near_payload_len;
    uint32_t ble_dbg_priv_seen;
    int8_t   ble_dbg_priv_rssi;
    char     ble_dbg_priv_label[24];
    char     ble_dbg_priv_name[32];
    char     ble_dbg_priv_reason[32];
    uint16_t ble_dbg_priv_cid;
    uint16_t ble_dbg_priv_svc0;
    uint8_t  ble_dbg_priv_svc_count;
    uint8_t  ble_dbg_priv_payload_len;
    uint32_t ble_host_restart_count;
    uint32_t ble_scan_start_count;
    uint32_t ble_scan_start_ok;
    int      ble_scan_last_rc;
    int      ble_sync_last_rc;
} ble_remote_id_stats_t;

bool ble_remote_id_is_scanning(void);
void ble_remote_id_get_stats(ble_remote_id_stats_t *out);
void ble_remote_id_reset_profile_counters(void);
uint32_t ble_remote_id_service_seen_count(void);
uint32_t ble_remote_id_emit_count(void);
uint32_t ble_remote_id_privacy_seen_count(void);

/**
 * Focus BLE reporting on a specific advertiser MAC.
 * Calibration mode cancels this focus to avoid suppressing the phone beacon.
 */
void ble_rid_lockon(const uint8_t mac[6], int duration_s);

/**
 * Cancel BLE focus mode and resume normal advertiser reporting.
 */
void ble_rid_lockon_cancel(void);

#if CONFIG_FOF_GLASSES_DETECTION
/**
 * Attach a queue for smart glasses / privacy device detections.
 * Must be called after ble_remote_id_init() and before ble_remote_id_start().
 *
 * @param queue FreeRTOS queue for glasses_detection_t results
 */
void ble_remote_id_set_glasses_queue(QueueHandle_t queue);
#endif

#ifdef __cplusplus
}
#endif
