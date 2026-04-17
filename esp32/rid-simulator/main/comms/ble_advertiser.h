#pragma once

/**
 * Friend or Foe — BLE ODID Advertiser
 *
 * NimBLE broadcaster that transmits OpenDroneID advertisements for up to
 * MAX_DRONES simulated drones IN PARALLEL via extended advertising sets.
 * Each drone has its own advertising instance, so both are always on-air
 * at ~1 Hz — matching real ASTM F3411 broadcast behavior.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Upper bound on concurrent drones — must match CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES. */
#define BLE_ADV_MAX_DRONES   2

/**
 * Initialize NimBLE and configure `num_drones` extended advertising instances
 * (one per simulated drone). Call once before ble_advertiser_start().
 * Caller is responsible for pushing message data via ble_advertiser_update_drone().
 */
void ble_advertiser_init(int num_drones);

/**
 * Start the NimBLE host task and kick off the rotation task. Both instances
 * begin advertising as soon as the host syncs.
 */
void ble_advertiser_start(void);

/**
 * Push the latest 4 ODID messages for drone `drone_idx`. Subsequent rotations
 * of that instance will pick up the new values.
 *
 * @param drone_idx      0-based drone index (< num_drones passed to init)
 * @param basic_id_msg   25-byte Basic ID ODID message
 * @param location_msg   25-byte Location ODID message (holds GPS; update every tick)
 * @param system_msg     25-byte System ODID message
 * @param operator_msg   25-byte Operator ID ODID message
 */
void ble_advertiser_update_drone(int drone_idx,
                                  const uint8_t *basic_id_msg,
                                  const uint8_t *location_msg,
                                  const uint8_t *system_msg,
                                  const uint8_t *operator_msg);

/** Get total advertisements emitted across all instances. */
uint32_t ble_advertiser_get_adv_count(void);

#ifdef __cplusplus
}
#endif
