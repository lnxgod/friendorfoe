/**
 * Friend or Foe — BLE-JA3 Fingerprinting
 *
 * Computes a structural hash of BLE advertisement data, similar to
 * JA3 for TLS. All devices of the same model produce the same hash
 * regardless of MAC rotation or payload key changes.
 *
 * Hash includes: AD type sequence, company ID, service UUIDs,
 * advertisement properties, payload length class, TX power, appearance.
 *
 * Hash excludes: MAC address, rotating Apple Continuity keys,
 * encrypted payloads, battery/state bytes.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_gap.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t value;     /* 32-bit FNV-1a hash */
    char     hex[9];    /* 8-char hex string + null */
} ble_ja3_hash_t;

/**
 * Compute BLE-JA3 hash from a GAP discovery event.
 * Works with both legacy (BLE_GAP_EVENT_DISC) and extended
 * (BLE_GAP_EVENT_EXT_DISC) advertisements.
 *
 * @param event  NimBLE GAP event
 * @param out    Output hash structure
 * @return true on success
 */
#ifdef CONFIG_BT_NIMBLE_ENABLED
bool ble_ja3_from_gap_event(const struct ble_gap_event *event,
                            ble_ja3_hash_t *out);
#endif

#ifdef __cplusplus
}
#endif
