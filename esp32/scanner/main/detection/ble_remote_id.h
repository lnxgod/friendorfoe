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
