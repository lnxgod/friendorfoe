#pragma once

/**
 * Friend or Foe — BLE ODID Advertiser
 *
 * NimBLE broadcaster that cycles through OpenDroneID message types.
 * Advertises service data on UUID 0xFFFA matching ASTM F3411 BLE format.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize NimBLE stack for broadcasting.
 * Call once before ble_advertiser_start().
 */
void ble_advertiser_init(void);

/**
 * Start the NimBLE host task (begins advertising on sync).
 */
void ble_advertiser_start(void);

/**
 * Update the ODID message data to advertise.
 * The advertiser will cycle through all 4 messages, switching every ~100ms.
 *
 * @param basic_id_msg   25-byte Basic ID ODID message
 * @param location_msg   25-byte Location ODID message
 * @param system_msg     25-byte System ODID message
 * @param operator_msg   25-byte Operator ID ODID message
 */
void ble_advertiser_update_messages(const uint8_t *basic_id_msg,
                                     const uint8_t *location_msg,
                                     const uint8_t *system_msg,
                                     const uint8_t *operator_msg);

/** Get count of advertisements sent. */
uint32_t ble_advertiser_get_adv_count(void);

#ifdef __cplusplus
}
#endif
